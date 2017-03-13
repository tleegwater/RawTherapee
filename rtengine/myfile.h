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
#pragma once

#include <memory>

#include <glib/gstdio.h>

namespace rtengine
{

    class ProgressListener;

}

struct IMFILE {
    int fd;
    ssize_t pos;
    ssize_t size;
    char* data;
    bool eof;
    rtengine::ProgressListener *plistener;
    double progress_range;
    ssize_t progress_next;
    ssize_t progress_current;

    IMFILE();
    ~IMFILE();
};

/*
  Functions for progress bar updates
  Note: progress bar is not intended to be exact, eg if you read same data over and over again progress
  will potentially reach 100% before you're finished.
 */
void imfile_set_plistener(const std::shared_ptr<IMFILE>& f, rtengine::ProgressListener *plistener, double progress_range);
void imfile_update_progress(const std::shared_ptr<IMFILE>& f);

std::shared_ptr<IMFILE> fopen(const char* fname);
std::shared_ptr<IMFILE> gfopen(const char* fname);
std::shared_ptr<IMFILE> fopen(unsigned* buf, int size);
void fclose(const std::shared_ptr<IMFILE>& f);

int ftell(const std::shared_ptr<IMFILE>& f);
int feof(const std::shared_ptr<IMFILE>& f);
void fseek(const std::shared_ptr<IMFILE>& f, int p, int how);

int fgetc(const std::shared_ptr<IMFILE>& f);
int getc(const std::shared_ptr<IMFILE>& f);

int fread(void* dst, int es, int count, const std::shared_ptr<IMFILE>& f);
unsigned char* fdata(int offset, const std::shared_ptr<IMFILE>& f);

int fscanf(const std::shared_ptr<IMFILE>& f, const char* s ...);
char* fgets(char* s, int n, const std::shared_ptr<IMFILE>& f);
