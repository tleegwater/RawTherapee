/*
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

#include <cmath>
#include <cassert>

#include "rawimagesource.h"
#include "rawimage.h"
#include "rt_math.h"
#include "../rtgui/multilangmgr.h"
#include "procparams.h"
#include "sleef.c"
#include "opthelper.h"
#define BENCHMARK
#include "StopWatch.h"

// If you want to use the code, you need to display name of the original authors in
// your software!

/* DCB demosaicing by Jacek Gozdz (cuniek@kft.umcs.lublin.pl)
 * the code is open source (BSD licence)
*/

using namespace std;

constexpr int TILESIZE = 192;
constexpr int TILEBORDER = 10;
constexpr int CACHESIZE = (TILESIZE+2*TILEBORDER);

namespace {

void dcb_initTileLimits(int W, int H, int x0, int y0, int border, int borderOffset, int &colMin, int &rowMin, int &colMax, int &rowMax)
{
    rowMin = border;
    colMin = border;
    rowMax = CACHESIZE - border;
    colMax = CACHESIZE - border;

    if(!y0 ) {
        rowMin = TILEBORDER + min(border, borderOffset);
    }

    if(!x0 ) {
        colMin = TILEBORDER + min(border, borderOffset);
    }

    if( y0 + TILESIZE + TILEBORDER >= H - min(border,borderOffset)) {
        rowMax = TILEBORDER + H - min(border, borderOffset) - y0;
    }

    if( x0 + TILESIZE + TILEBORDER >= W - min(border,borderOffset)) {
        colMax = TILEBORDER + W - min(border, borderOffset) - x0;
    }
}

}

namespace rtengine
{



void RawImageSource::dcb_fill_raw( float (*tile )[3], int x0, int y0, float** rawData)
{
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 0, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin, y = y0 - TILEBORDER + rowMin; row < rowMax; row++, y++)
        for (int col = colMin, x = x0 - TILEBORDER + colMin, indx = row * CACHESIZE + col; col < colMax; col++, x++, indx++) {
            tile[indx][ri->FC(y, x)] = rawData[y][x];
        }
}

void RawImageSource::dcb_fill_border( float (*tile )[3], int border, int x0, int y0)
{
    unsigned row, col, y, x, f, c;
    float sum[8];

    for (row = y0; row < y0 + TILESIZE + TILEBORDER && row < H; row++) {
        for (col = x0; col < x0 + TILESIZE + TILEBORDER && col < W; col++) {
            if (col >= border && col < W - border && row >= border && row < H - border) {
                col = W - border;

                if(col >= x0 + TILESIZE + TILEBORDER ) {
                    break;
                }
            }

            memset(sum, 0, sizeof sum);

            for (y = row - 1; y != row + 2; y++)
                for (x = col - 1; x != col + 2; x++)
                    if (y < H && y < y0 + TILESIZE + TILEBORDER && x < W && x < x0 + TILESIZE + TILEBORDER) {
                        f = ri->FC(y, x);
                        sum[f] += tile[(y - y0 + TILEBORDER) * CACHESIZE + TILEBORDER + x - x0][f];
                        sum[f + 4]++;
                    }

            f = ri->FC(row, col);
            for (int c=0; c < 3; c++) {
                if (c != f && sum[c + 4] > 0) {
                    tile[(row - y0 + TILEBORDER) * CACHESIZE + TILEBORDER + col - x0][c] = sum[c] / sum[c + 4];
                }
            }
        }
    }
}

// restore original red/blue values from rawData
void RawImageSource::dcb_restore_red_blue(float (*tile)[3], int x0, int y0, float** rawData)
{
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 0, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin, y = y0 - TILEBORDER + rowMin; row < rowMax; row++, y++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), x = x0 - TILEBORDER + col, indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col); col < colMax; col+=2, x+=2, indx+=2) {
            tile[indx][c] = rawData[y][x];
        }
}

// First pass green interpolation
void RawImageSource::dcb_hid(float (*tile)[3], int x0, int y0)
{
    constexpr int u = CACHESIZE;
    int rowMin, colMin, rowMax, colMax, c;
    dcb_initTileLimits(W, H, x0, y0, 2, 4, colMin, rowMin, colMax, rowMax);

// simple green bilinear in R and B pixels
    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col); col < colMax; col += 2, indx += 2) {
            assert(indx - u - 1 >= 0 && indx + u + 1 < u * u && c >= 0 && c < 3);

            tile[indx][1] = 0.25f * (tile[indx-1][1] + tile[indx+1][1] + tile[indx-u][1] + tile[indx+u][1]);
        }
}

// missing colours are interpolated
void RawImageSource::dcb_color(float (*tile)[3], int x0, int y0)
{
    constexpr int u = CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 1, 4, colMin, rowMin, colMax, rowMax);

    // red in blue pixel, blue in red pixel
    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), indx = row * CACHESIZE + col, c = 2 - ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col); col < colMax; col += 2, indx += 2) {
            assert(indx >= 0 && indx < u * u && c >= 0 && c < 3);

            tile[indx][c] = tile[indx][1] +
                               ( tile[indx + u + 1][c] + tile[indx + u - 1][c] + tile[indx - u + 1][c] + tile[indx - u - 1][c]
                              - (tile[indx + u + 1][1] + tile[indx + u - 1][1] + tile[indx - u + 1][1] + tile[indx - u - 1][1]) ) * 0.25f;
       }

    // red or blue in green pixels
    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin + 1) & 1), indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col + 1), d = 2 - c; col < colMax; col += 2, indx += 2) {
            assert(indx >= 0 && indx < u * u && c >= 0 && c < 3);

            tile[indx][c] = tile[indx][1] + (tile[indx + 1][c] + tile[indx - 1][c] - (tile[indx + 1][1] + tile[indx - 1][1])) * 0.5f;
            tile[indx][d] = tile[indx][1] + (tile[indx + u][d] + tile[indx - u][d] - (tile[indx + u][1] + tile[indx - u][1])) * 0.5f;
        }
}

// green correction
void RawImageSource::dcb_hid2(float (*tile)[3], int x0, int y0)
{
    constexpr int u = CACHESIZE, v = 2 * CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 2, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin; row < rowMax; row++) {
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col); col < colMax; col += 2, indx += 2) {
            assert(indx - v >= 0 && indx + v < u * u);

            tile[indx][1] = tile[indx][c] +
                             (tile[indx + v][1] + tile[indx - v][1] + tile[indx - 2][1] + tile[indx + 2][1]
                           - (tile[indx + v][c] + tile[indx - v][c] + tile[indx - 2][c] + tile[indx + 2][c])) * 0.25f;
        }
    }
}

// green is used to create
// an interpolation direction map
// 1 = vertical
// 0 = horizontal
// saved in map[]

// seems at least 2 persons implemented some code, as this one has different coding style, could be unified
// I don't know if *pix is faster than a loop working on tile[] directly
void RawImageSource::dcb_map(float (*tile)[3], uint8_t *map, int x0, int y0)
{
    constexpr int u = 3 * CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 2, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin; row < rowMax; row++) {
        for (int col = colMin, indx = row * CACHESIZE + col; col < colMax; col++, indx++) {
            float *pix = &(tile[indx][1]);

            assert(indx >= 0 && indx < u * u);

            // comparing 4 * a to (b+c+d+e) instead of a to (b+c+d+e)/4 is faster because divisions are slow
            if ( 4 * (*pix) > ( (pix[-3] + pix[+3]) + (pix[-u] + pix[+u])) ) {
                map[indx] = ((min(pix[-3], pix[+3]) + (pix[-3] + pix[+3]) ) < (min(pix[-u], pix[+u]) + (pix[-u] + pix[+u])));
            } else {
                map[indx] = ((max(pix[-3], pix[+3]) + (pix[-3] + pix[+3]) ) > (max(pix[-u], pix[+u]) + (pix[-u] + pix[+u])));
            }
        }
    }
}

// interpolated green pixels are corrected using the map
void RawImageSource::dcb_correction(float (*tile)[3], const uint8_t* map, int x0, int y0)
{
    constexpr int u = CACHESIZE, v = 2 * CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 2, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin; row < rowMax; row++) {
        for (int indx = row * CACHESIZE + colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1); indx < row * CACHESIZE + colMax; indx += 2) {
            float current = 4 * map[indx] +
                            2 * (map[indx + u] + map[indx - u] + map[indx + 1] + map[indx - 1]) +
                            map[indx + v] + map[indx - v] + map[indx + 2] + map[indx - 2];

            assert(indx >= 0 && indx < u * u);
            tile[indx][1] = ((16.f - current) * (tile[indx - 1][1] + tile[indx + 1][1]) + current * (tile[indx - u][1] + tile[indx + u][1]) ) * 0.03125f;
        }
    }
}

// R and B smoothing using green contrast, all pixels except 2 pixel wide border

// again code with *pix, is this kind of calculating faster in C, than this what was commented?
void RawImageSource::dcb_pp(float (*tile)[3], int x0, int y0)
{
    constexpr int u = CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 2, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin, indx = row * CACHESIZE + col; col < colMax; col++, indx++) {
//            float r1 = tile[indx-1][0] + tile[indx+1][0] + tile[indx-u][0] + tile[indx+u][0] + tile[indx-u-1][0] + tile[indx+u+1][0] + tile[indx-u+1][0] + tile[indx+u-1][0];
//            float g1 = tile[indx-1][1] + tile[indx+1][1] + tile[indx-u][1] + tile[indx+u][1] + tile[indx-u-1][1] + tile[indx+u+1][1] + tile[indx-u+1][1] + tile[indx+u-1][1];
//            float b1 = tile[indx-1][2] + tile[indx+1][2] + tile[indx-u][2] + tile[indx+u][2] + tile[indx-u-1][2] + tile[indx+u+1][2] + tile[indx-u+1][2] + tile[indx+u-1][2];
            float (*pix)[3] = tile + (indx - u - 1);
            float r1 = (*pix)[0];
            float g1 = (*pix)[1];
            float b1 = (*pix)[2];
            pix++;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            pix++;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            pix += CACHESIZE - 2;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            pix += 2;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            pix += CACHESIZE - 2;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            pix++;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            pix++;
            r1 += (*pix)[0];
            g1 += (*pix)[1];
            b1 += (*pix)[2];
            r1 *= 0.125f;
            g1 *= 0.125f;
            b1 *= 0.125f;
            r1 += ( tile[indx][1] - g1 );
            b1 += ( tile[indx][1] - g1 );

            assert(indx >= 0 && indx < u * u);
            tile[indx][0] = r1;
            tile[indx][2] = b1;
        }
}

// interpolated green pixels are corrected using the map
// with correction
void RawImageSource::dcb_correction2(float (*tile)[3], const uint8_t* map, int x0, int y0)
{
    constexpr int u = CACHESIZE, v = 2 * CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 4, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin; row < rowMax; row++) {
        for (int indx = row * CACHESIZE + colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1)); indx < row * CACHESIZE + colMax; indx += 2) {
            // map values are uint8_t either 0 or 1. Adding them using integer instructions is perfectly valid and fast. Final result is converted to float then
            float current = 4 * map[indx] +
                            2 * (map[indx + u] + map[indx - u] + map[indx + 1] + map[indx - 1]) +
                            map[indx + v] + map[indx - v] + map[indx + 2] + map[indx - 2];

            assert(indx >= 0 && indx < u * u);

            tile[indx][1] =  tile[indx][c] +
                    ((16.f - current) * (tile[indx - 1][1] + tile[indx + 1][1] - (tile[indx + 2][c] + tile[indx - 2][c]))
                            + current * (tile[indx - u][1] + tile[indx + u][1] - (tile[indx + v][c] + tile[indx - v][c]))) * 0.03125f;
        }
    }
}

// tile refinement
void RawImageSource::dcb_refinement(float (*tile)[3], const uint8_t* map, int x0, int y0)
{
    constexpr int u = CACHESIZE, v = 2 * CACHESIZE, w = 3 * CACHESIZE;
    int rowMin, colMin, rowMax, colMax;
    dcb_initTileLimits(W, H, x0, y0, 4, 4, colMin, rowMin, colMax, rowMax);

    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col); col < colMax; col += 2, indx += 2) {

            float current = 4 * map[indx] +
                            2 * (map[indx + u] + map[indx - u] + map[indx + 1] + map[indx - 1])
                            + map[indx + v] + map[indx - v] + map[indx - 2] + map[indx + 2];

            float currPix = tile[indx][c];

            float v0 = (tile[indx - u][1] + tile[indx + u][1]) / (1.f + 2.f * currPix);
            float v1 = 2.f * tile[indx - u][1] / (1.f + tile[indx - v][c] + currPix);
            float v2 = 2.f * tile[indx + u][1] / (1.f + tile[indx + v][c] + currPix);

            float gv = v0 + v1 + v2;

            float h0 = (tile[indx - 1][1] + tile[indx + 1][1]) / (1.f + 2.f * currPix);
            float h1 = 2.f * tile[indx - 1][1] / (1.f + tile[indx - 2][c] + currPix);
            float h2 = 2.f * tile[indx + 1][1] / (1.f + tile[indx + 2][c] + currPix);

            float gh = h0 + h1 + h2;

            // new green value
            assert(indx >= 0 && indx < u * u);
            currPix *= (current * gv + (16.f - current) * gh) / 48.f;

            // get rid of the overshot pixels
            float minVal = min(tile[indx - 1][1], tile[indx + 1][1], tile[indx - u][1], tile[indx + u][1]);
            float maxVal = max(tile[indx - 1][1], tile[indx + 1][1], tile[indx - u][1], tile[indx + u][1]);

            tile[indx][1] =  LIM(currPix, minVal, maxVal);

        }
}

// missing colours are interpolated using high quality algorithm by Luis Sanz Rodriguez
void RawImageSource::dcb_color_full(float (*tile)[3], int x0, int y0, float (*chroma)[2])
{
    constexpr int u = CACHESIZE, w = 3 * CACHESIZE;
    int rowMin, colMin, rowMax, colMax;

    // we need the tile with 6 px border for next step, and (for whatever reason) with 9 px border if the tile is a border tile
    dcb_initTileLimits(W, H, x0, y0, TILEBORDER - 6, -9, colMin, rowMin, colMax, rowMax);

    float f[4], g[4];

    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col), d = c / 2; col < colMax; col += 2, indx += 2) {
            assert(indx >= 0 && indx < u * u && c >= 0 && c < 3);
            chroma[indx][d] = tile[indx][c] - tile[indx][1];
        }

    // we need the tile with 3 px border for next step
    dcb_initTileLimits(W, H, x0, y0, TILEBORDER - 3, 3, colMin, rowMin, colMax, rowMax);
    for (int row = rowMin; row < rowMax; row++)
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin) & 1), indx = row * CACHESIZE + col, c = 1 - ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col) / 2; col < colMax; col += 2, indx += 2) {
            f[0] = 1.f / (1.f + fabs(chroma[indx - u - 1][c] - chroma[indx + u + 1][c]) + fabs(chroma[indx - u - 1][c] - chroma[indx - w - 3][c]) + fabs(chroma[indx + u + 1][c] - chroma[indx - w - 3][c]));
            f[1] = 1.f / (1.f + fabs(chroma[indx - u + 1][c] - chroma[indx + u - 1][c]) + fabs(chroma[indx - u + 1][c] - chroma[indx - w + 3][c]) + fabs(chroma[indx + u - 1][c] - chroma[indx - w + 3][c]));
            f[2] = 1.f / (1.f + fabs(chroma[indx + u - 1][c] - chroma[indx - u + 1][c]) + fabs(chroma[indx + u - 1][c] - chroma[indx + w + 3][c]) + fabs(chroma[indx - u + 1][c] - chroma[indx + w - 3][c]));
            f[3] = 1.f / (1.f + fabs(chroma[indx + u + 1][c] - chroma[indx - u - 1][c]) + fabs(chroma[indx + u + 1][c] - chroma[indx + w - 3][c]) + fabs(chroma[indx - u - 1][c] - chroma[indx + w + 3][c]));
            g[0] = 1.325f * chroma[indx - u - 1][c] - 0.175f * chroma[indx - w - 3][c] - 0.075f * (chroma[indx - w - 1][c] + chroma[indx - u - 3][c]);
            g[1] = 1.325f * chroma[indx - u + 1][c] - 0.175f * chroma[indx - w + 3][c] - 0.075f * (chroma[indx - w + 1][c] + chroma[indx - u + 3][c]);
            g[2] = 1.325f * chroma[indx + u - 1][c] - 0.175f * chroma[indx + w - 3][c] - 0.075f * (chroma[indx + w - 1][c] + chroma[indx + u - 3][c]);
            g[3] = 1.325f * chroma[indx + u + 1][c] - 0.175f * chroma[indx + w + 3][c] - 0.075f * (chroma[indx + w + 1][c] + chroma[indx + u + 3][c]);

            assert(indx >= 0 && indx < u * u && c >= 0 && c < 2);
            chroma[indx][c] = (f[0] * g[0] + f[1] * g[1] + f[2] * g[2] + f[3] * g[3]) / (f[0] + f[1] + f[2] + f[3]);
        }

    // we only need the tile without border for next step
    dcb_initTileLimits(W, H, x0, y0, TILEBORDER, 4, colMin, rowMin, colMax, rowMax);
    for (int row = rowMin; row < rowMax; row++) {
        for (int col = colMin + (ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + colMin + 1) & 1), indx = row * CACHESIZE + col, c = ri->FC(y0 - TILEBORDER + row, x0 - TILEBORDER + col + 1) / 2; col < colMax; col += 2, indx += 2) {
            for(int d = 0; d <= 1; c = 1 - c, d++) {
                f[0] = 1.f / (1.f + fabs(chroma[indx - u][c] - chroma[indx + u][c]) + fabs(chroma[indx - u][c] - chroma[indx - w][c]) + fabs(chroma[indx + u][c] - chroma[indx - w][c]));
                f[1] = 1.f / (1.f + fabs(chroma[indx + 1][c] - chroma[indx - 1][c]) + fabs(chroma[indx + 1][c] - chroma[indx + 3][c]) + fabs(chroma[indx - 1][c] - chroma[indx + 3][c]));
                f[2] = 1.f / (1.f + fabs(chroma[indx - 1][c] - chroma[indx + 1][c]) + fabs(chroma[indx - 1][c] - chroma[indx - 3][c]) + fabs(chroma[indx + 1][c] - chroma[indx - 3][c]));
                f[3] = 1.f / (1.f + fabs(chroma[indx + u][c] - chroma[indx - u][c]) + fabs(chroma[indx + u][c] - chroma[indx + w][c]) + fabs(chroma[indx - u][c] - chroma[indx + w][c]));

                g[0] = intp(0.875f, chroma[indx - u][c], chroma[indx - w][c]);
                g[1] = intp(0.875f, chroma[indx + 1][c], chroma[indx + 3][c]);
                g[2] = intp(0.875f, chroma[indx - 1][c], chroma[indx - 3][c]);
                g[3] = intp(0.875f, chroma[indx + u][c], chroma[indx + w][c]);

                assert(indx >= 0 && indx < u * u && c >= 0 && c < 2);
                chroma[indx][c] = (f[0] * g[0] + f[1] * g[1] + f[2] * g[2] + f[3] * g[3]) / (f[0] + f[1] + f[2] + f[3]);
            }
        }
    }

    // we only need the tile without border for next step
    dcb_initTileLimits(W, H, x0, y0, TILEBORDER, 4, colMin, rowMin, colMax, rowMax);

    for(int row = rowMin; row < rowMax; row++)
        for(int col = colMin, indx = row * CACHESIZE + col; col < colMax; col++, indx++) {
            assert(indx >= 0 && indx < u * u);

            tile[indx][0] = chroma[indx][0] + tile[indx][1];
            tile[indx][2] = chroma[indx][1] + tile[indx][1];
        }
}

// DCB demosaicing main routine
void RawImageSource::dcb_demosaic(int iterations, bool dcb_enhance)
{
BENCHFUN
    double currentProgress = 0.0;

    if(plistener) {
        plistener->setProgressStr (Glib::ustring::compose(M("TP_RAW_DMETHOD_PROGRESSBAR"), RAWParams::BayerSensor::methodstring[RAWParams::BayerSensor::dcb]));
        plistener->setProgress (currentProgress);
    }

    int wTiles = W / TILESIZE + (W % TILESIZE ? 1 : 0);
    int hTiles = H / TILESIZE + (H % TILESIZE ? 1 : 0);
    int numTiles = wTiles * hTiles;
    int tilesDone = 0;
    constexpr int cldf = 2; // factor to multiply cache line distance. 1 = 64 bytes, 2 = 128 bytes ...

#ifdef _OPENMP
    #pragma omp parallel
#endif
{
    // assign working space
    char *buffer = (char *) malloc(5 * sizeof(float) * CACHESIZE * CACHESIZE + 1 * cldf * 64 + 63);
    // aligned to 64 byte boundary
    char *data = (char*)( ( uintptr_t(buffer) + uintptr_t(63)) / 64 * 64);

    float (*tile)[3] = (float(*)[3]) data;
    float (*chrm)[2] = (float(*)[2]) ((char*)tile + sizeof(float) * CACHESIZE * CACHESIZE * 3 + cldf * 64);
    uint8_t *map     = (uint8_t*) chrm;  // No overlap in usage of map and chrm means we can reuse chrm

#ifdef _OPENMP
    #pragma omp for schedule(dynamic) nowait
#endif

    for( int iTile = 0; iTile < numTiles; iTile++) {
        int xTile = iTile % wTiles;
        int yTile = iTile / wTiles;
        int x0 = xTile * TILESIZE;
        int y0 = yTile * TILESIZE;

        memset(tile, 0, CACHESIZE * CACHESIZE * sizeof * tile);
        memset(map, 0, CACHESIZE * CACHESIZE * sizeof * map);

        dcb_fill_raw( tile, x0, y0, rawData );

        if( !xTile || !yTile || xTile == wTiles - 1 || yTile == hTiles - 1) {
            dcb_fill_border(tile, 6, x0, y0);
        }

        dcb_hid(tile, x0, y0);

        for (int i = iterations; i > 0; i--) {
            dcb_hid2(tile, x0, y0);
            dcb_hid2(tile, x0, y0);
            dcb_hid2(tile, x0, y0);
            dcb_map(tile, map, x0, y0);
            dcb_correction(tile, map, x0, y0);
        }

        dcb_color(tile, x0, y0);
        dcb_pp(tile, x0, y0);
        dcb_map(tile, map, x0, y0);
        dcb_correction2(tile, map, x0, y0);
        dcb_map(tile, map, x0, y0);
        dcb_correction(tile, map, x0, y0);
        dcb_color(tile, x0, y0);
        dcb_map(tile, map, x0, y0);
        dcb_correction(tile, map, x0, y0);
        dcb_map(tile, map, x0, y0);
        dcb_correction(tile, map, x0, y0);
        dcb_map(tile, map, x0, y0);
        dcb_restore_red_blue(tile, x0, y0, rawData);

        if (!dcb_enhance)
            dcb_color(tile, x0, y0);
        else
        {
            dcb_refinement(tile, map, x0, y0);
            if( !xTile || !yTile || xTile == wTiles - 1 || yTile == hTiles - 1) {
                // clearing chrm only needed for border tiles
                memset(chrm, 0, CACHESIZE * CACHESIZE * sizeof * chrm);
            }
            dcb_color_full(tile, x0, y0, chrm);
        }

         for(int y = 0; y < TILESIZE && y0 + y < H; y++) {
            for (int j = 0; j < TILESIZE && x0 + j < W; j++) {
                red[y0 + y][x0 + j]   = tile[(y + TILEBORDER) * CACHESIZE + TILEBORDER + j][0];
                green[y0 + y][x0 + j] = tile[(y + TILEBORDER) * CACHESIZE + TILEBORDER + j][1];
                blue[y0 + y][x0 + j]  = tile[(y + TILEBORDER) * CACHESIZE + TILEBORDER + j][2];
            }
        }

        if( plistener && double(tilesDone) / numTiles > currentProgress) {
#ifdef _OPENMP
            #pragma omp critical (dcbprogress)
#endif
            {
                currentProgress += 0.05; // Show progress each 5%
                plistener->setProgress (currentProgress);
            }
        }

#ifdef _OPENMP
        #pragma omp atomic
#endif
        tilesDone++;
    }
    free(buffer);
}

    if(plistener) {
        plistener->setProgress (1.0);
    }
}

}