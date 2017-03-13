/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <glibmm.h>
#ifdef BZIP_SUPPORT
#include <bzlib.h>
#endif

#include "myfile.h"
#include "rtengine.h"

#include "../rtgui/threadutils.h"

// get mmap() sorted out
#ifdef MYFILE_MMAP

#ifdef WIN32

#include <fcntl.h>
#include <windows.h>

// dummy values
#define MAP_PRIVATE 1
#define PROT_READ 1
#define MAP_FAILED (void *)-1

void* mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
    HANDLE handle = CreateFileMapping((HANDLE)_get_osfhandle(fd), NULL, PAGE_WRITECOPY, 0, 0, NULL);

    if (handle != NULL) {
        start = MapViewOfFile(handle, FILE_MAP_COPY, 0, offset, length);
        CloseHandle(handle);
        return start;
    }

    return MAP_FAILED;
}

int munmap(void *start, size_t length)
{
    UnmapViewOfFile(start);
    return 0;
}

#else // WIN32

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#endif // WIN32
#endif // MYFILE_MMAP


namespace
{

class IrretrievableFileStore final
{
public:
    static IrretrievableFileStore& getInstance()
    {
        static IrretrievableFileStore instance;
        return instance;
    }

    void insert(const std::string& name, const std::shared_ptr<IMFILE>& file)
    {
        const MyMutex::MyLock lock(mutex);
        items[name] = file;
    }

    bool get(const std::string& name, std::shared_ptr<IMFILE>& file) const
    {
        const MyMutex::MyLock lock(mutex);
        const std::map<std::string, std::shared_ptr<IMFILE>>::const_iterator item = items.find(name);
        if (item != items.end()) {
            file = item->second;
            file->pos = 0;
            file->eof = false;
            return true;
        }
        return false;
    }

private:
    IrretrievableFileStore() = default;

    std::map<std::string, std::shared_ptr<IMFILE>> items;
    mutable MyMutex mutex;
};

std::shared_ptr<IMFILE> fopenStd(const char* fname)
{
    FILE* const f = g_fopen(fname, "rb");

    if (!f) {
        return std::shared_ptr<IMFILE>();
    }

    const std::shared_ptr<IMFILE> mf = std::make_shared<IMFILE>();
    if (!fseek(f, 0, SEEK_END)) {
        mf->size = ftell(f);
        mf->data = new char[mf->size];
        fseek(f, 0, SEEK_SET);
        fread(mf->data, 1, mf->size, f);
    } else {
        mf->fd = -1;

        std::string data;
        char buf[4096];
        size_t read = fread(buf, 1, sizeof(buf), f);
        data.append(buf, read);
        while (!feof(f) && read == sizeof(buf)) {
            read = fread(buf, 1, sizeof(buf), f);
            data.append(buf, read);
        }

        mf->size = data.size();
        mf->data = new char[mf->size];
        data.copy(mf->data, data.size());

        IrretrievableFileStore::getInstance().insert(fname, mf);
    }
    fclose(f);
    mf->pos = 0;
    mf->eof = false;

    return mf;
}


}

IMFILE::IMFILE() :
    fd(0),
    pos(0),
    size(0),
    data(nullptr),
    eof(0),
    plistener(nullptr),
    progress_range(0),
    progress_next(0),
    progress_current(0)
{
}

IMFILE::~IMFILE()
{
#ifdef MYFILE_MMAP

    if (fd == -1) {
        delete[] data;
    } else {
        munmap((void*)data, size);
        close(fd);
    }

#else
    delete[] data;
#endif
}

#ifdef MYFILE_MMAP

std::shared_ptr<IMFILE> fopen(const char* fname)
{
    printf("get '%s'\n", fname);
    std::shared_ptr<IMFILE> mf;
    if (IrretrievableFileStore::getInstance().get(fname, mf)) {
        return mf;
    }

    int fd;

#ifdef WIN32

    fd = -1;
    // First convert UTF8 to UTF16, then use Windows function to open the file and convert back to file descriptor.
    std::unique_ptr<wchar_t, GFreeFunc> wfname (reinterpret_cast<wchar_t*>(g_utf8_to_utf16 (fname, -1, NULL, NULL, NULL)), g_free);

    HANDLE hFile = CreateFileW (wfname.get (), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        fd = _open_osfhandle((intptr_t)hFile, 0);
    }

#else

    fd = ::g_open (fname, O_RDONLY);

#endif

    if (fd < 0) {
        return nullptr;
    }

    struct stat stat_buffer;

    if (fstat(fd, &stat_buffer) < 0) {
        printf("no stat\n");
        close (fd);
        return nullptr;
    }

    void* const data = mmap(nullptr, stat_buffer.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (data == MAP_FAILED) {
        printf("no mmap\n");
        close(fd);
        return fopenStd(fname);
    }

    mf = std::make_shared<IMFILE>();

    mf->fd = fd;
    mf->pos = 0;
    mf->size = stat_buffer.st_size;
    mf->data = static_cast<char*>(data);
    mf->eof = false;

    return mf;
}

std::shared_ptr<IMFILE> gfopen(const char* fname)
{
    const std::shared_ptr<IMFILE> mf = fopen(fname);

#ifdef BZIP_SUPPORT
    {
        if (rtengine::getFileExtension(fname) == "bz2") {
            int ret;

            // initialize bzip stream structure
            bz_stream stream;
            stream.bzalloc = nullptr;
            stream.bzfree = nullptr;
            stream.opaque = nullptr;
            ret = BZ2_bzDecompressInit(&stream, 0, 0);

            if (ret != BZ_OK) {
                printf("bzip initialization failed with error %d\n", ret);
            } else {
                // allocate initial buffer for decompressed data
                unsigned int buffer_out_count = 0; // bytes of decompressed data
                unsigned int buffer_size = 10 * 1024 * 1024; // 10 MB, extended dynamically if needed
                char* buffer = nullptr;

                stream.next_in = mf->data; // input data address
                stream.avail_in = mf->size;

                while (ret == BZ_OK) {
                    buffer = static_cast<char*>( realloc(buffer, buffer_size)); // allocate/resize buffer

                    if (!buffer) {
                        free(buffer);
                    }

                    stream.next_out = buffer + buffer_out_count; // output data adress
                    stream.avail_out = buffer_size - buffer_out_count;

                    ret = BZ2_bzDecompress(&stream);

                    buffer_size *= 2; // increase buffer size for next iteration
                    buffer_out_count = stream.total_out_lo32;

                    if (stream.total_out_hi32 > 0) {
                        printf("bzip decompressed data byte count high byte is nonzero: %d\n", stream.total_out_hi32);
                    }
                }

                if (ret == BZ_STREAM_END) {
                    //delete [] mf->data;
                    // close memory mapping, setting fd -1 will ensure deletion of mf->data upon fclose()
                    mf->fd = -1;
                    munmap((void*)mf->data, mf->size);
                    close(mf->fd);

                    char* realData = new char [buffer_out_count];
                    memcpy(realData, buffer, buffer_out_count);

                    mf->data = realData;
                    mf->size = buffer_out_count;
                } else {
                    printf("bzip decompression failed with error %d\n", ret);
                }

                // cleanup
                free(buffer);
                ret = BZ2_bzDecompressEnd(&stream);

                if (ret != BZ_OK) {
                    printf("bzip cleanup failed with error %d\n", ret);
                }
            }
        }
    }
#endif // BZIP_SUPPORT

    return mf;
}

#else // MYFILE_MMAP

std::shared_ptr<IMFILE> fopen(const char* fname)
{
    printf("get '%s'\n", fname);
    std::shared_ptr<IMFILE> mf;
    if (IrretrievableFileStore::getInstance().get(fname, mf)) {
        return mf;
    }

    return fopenStd(fname);
}

std::shared_ptr<IMFILE> gfopen(const char* fname)
{
    const std::shared_ptr<IMFILE> mf = fopen(fname);

#ifdef BZIP_SUPPORT
    {
        if (rtengine::getFileExtension(fname) == "bz2") {
            int ret;

            // initialize bzip stream structure
            bz_stream stream;
            stream.bzalloc = 0;
            stream.bzfree = 0;
            stream.opaque = 0;
            ret = BZ2_bzDecompressInit(&stream, 0, 0);

            if (ret != BZ_OK) {
                printf("bzip initialization failed with error %d\n", ret);
            } else {
                // allocate initial buffer for decompressed data
                unsigned int buffer_out_count = 0; // bytes of decompressed data
                unsigned int buffer_size = 10 * 1024 * 1024; // 10 MB, extended dynamically if needed
                char* buffer = 0;

                stream.next_in = mf->data; // input data address
                stream.avail_in = mf->size;

                while (ret == BZ_OK) {
                    buffer = static_cast<char*>(realloc(buffer, buffer_size)); // allocate/resize buffer

                    if (!buffer) {
                        free(buffer);
                    }

                    stream.next_out = buffer + buffer_out_count; // output data adress
                    stream.avail_out = buffer_size - buffer_out_count;

                    ret = BZ2_bzDecompress(&stream);

                    buffer_size *= 2; // increase buffer size for next iteration
                    buffer_out_count = stream.total_out_lo32;

                    if (stream.total_out_hi32 > 0) {
                        printf("bzip decompressed data byte count high byte is nonzero: %d\n", stream.total_out_hi32);
                    }
                }

                if (ret == BZ_STREAM_END) {
                    delete[] mf->data;
                    char* realData = new char[buffer_out_count];
                    memcpy(realData, buffer, buffer_out_count);

                    mf->data = realData;
                    mf->size = buffer_out_count;
                } else {
                    printf("bzip decompression failed with error %d\n", ret);
                }

                // cleanup
                free(buffer);
                ret = BZ2_bzDecompressEnd(&stream);

                if (ret != BZ_OK) {
                    printf("bzip cleanup failed with error %d\n", ret);
                }
            }
        }
    }
#endif // BZIP_SUPPORT

    return mf;
}
#endif //MYFILE_MMAP

std::shared_ptr<IMFILE> fopen(unsigned* buf, int size)
{
    const std::shared_ptr<IMFILE> mf = std::make_shared<IMFILE>();
    mf->fd = -1;
    mf->size = size;
    mf->data = new char [mf->size];
    memcpy ((void*)mf->data, buf, size);
    mf->pos = 0;
    mf->eof = false;
    return mf;
}

void fclose(const std::shared_ptr<IMFILE>& f)
{
}

int ftell(const std::shared_ptr<IMFILE>& f)
{
    return f->pos;
}

int feof(const std::shared_ptr<IMFILE>& f)
{
    return f->eof;
}

void fseek(const std::shared_ptr<IMFILE>& f, int p, int how)
{
    const int fpos = f->pos;

    if (how == SEEK_SET) {
        f->pos = p;
    } else if (how == SEEK_CUR) {
        f->pos += p;
    } else if (how == SEEK_END) {
        f->pos = f->size + p;
    }

    if (f->pos < 0  || f->pos > f->size) {
        f->pos = fpos;
    }
}

int fgetc(const std::shared_ptr<IMFILE>& f)
{
    if (f->pos < f->size) {
        if (f->plistener && ++f->progress_current >= f->progress_next) {
            imfile_update_progress(f);
        }

        return static_cast<unsigned char>(f->data[f->pos++]);
    }

    f->eof = true;
    return EOF;
}

int getc(const std::shared_ptr<IMFILE>& f)
{
    return fgetc(f);
}

int fread(void* dst, int es, int count, const std::shared_ptr<IMFILE>& f)
{
    const int s = es * count;
    const int avail = f->size - f->pos;

    if (s <= avail) {
        memcpy(dst, f->data + f->pos, s);
        f->pos += s;

        if (f->plistener) {
            f->progress_current += s;

            if (f->progress_current >= f->progress_next) {
                imfile_update_progress(f);
            }
        }

        return count;
    } else {
        memcpy(dst, f->data + f->pos, avail);
        f->pos += avail;
        f->eof = true;
        return avail / es;
    }
}

unsigned char* fdata(int offset, const std::shared_ptr<IMFILE>& f)
{
    return reinterpret_cast<unsigned char*>(f->data + offset);
}

int fscanf(const std::shared_ptr<IMFILE>& f, const char* s ...)
{
    // fscanf not easily wrapped since we have no terminating \0 at end
    // of file data and vsscanf() won't tell us how many characters that
    // were parsed. However, only dcraw.cc code use it and only for "%f" and
    // "%d", so we make a dummy fscanf here just to support dcraw case.

    char buf[50], *endptr = nullptr;
    int copy_sz = f->size - f->pos;

    if (copy_sz > sizeof(buf)) {
        copy_sz = sizeof(buf) - 1;
    }

    memcpy(buf, &f->data[f->pos], copy_sz);
    buf[copy_sz] = '\0';
    va_list ap;
    va_start (ap, s);

    if (strcmp(s, "%d") == 0) {
        int i = strtol(buf, &endptr, 10);

        if (endptr == buf) {
            va_end (ap);
            return 0;
        }

        int *pi = va_arg(ap, int*);
        *pi = i;
    } else if (strcmp(s, "%f") == 0) {
        float f = strtof(buf, &endptr);

        if (endptr == buf) {
            va_end (ap);
            return 0;
        }

        float *pf = va_arg(ap, float*);
        *pf = f;
    }

    va_end (ap);
    f->pos += endptr - buf;
    return 1;
}


char* fgets(char* s, int n, const std::shared_ptr<IMFILE>& f)
{
    if (f->pos >= f->size) {
        f->eof = true;
        return nullptr;
    }

    int i = 0;

    do {
        s[i++] = f->data[f->pos++];
    } while (i < n && f->pos < f->size);

    return s;
}

void imfile_set_plistener(const std::shared_ptr<IMFILE>& f, rtengine::ProgressListener *plistener, double progress_range)
{
    f->plistener = plistener;
    f->progress_range = progress_range;
    f->progress_next = f->size / 10 + 1;
    f->progress_current = 0;
}

void imfile_update_progress(const std::shared_ptr<IMFILE>& f)
{
    if (!f->plistener || f->progress_current < f->progress_next) {
        return;
    }

    do {
        f->progress_next += f->size / 10 + 1;
    } while (f->progress_next < f->progress_current);

    double p = (double)f->progress_current / f->size;

    if (p > 1.0) {
        /* this can happen if same bytes are read over and over again. Progress bar is not intended
           to be exact, just give some progress indication for normal raw file access patterns */
        p = 1.0;
    }

    f->plistener->setProgress(p * f->progress_range);
}
