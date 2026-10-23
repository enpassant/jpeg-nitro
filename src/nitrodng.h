// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrodng: develops the raw image of a DNG file (Adobe Digital Negative) into 8-bit sRGB,
// written from the DNG specification, without dependencies.
//
// Raw data: uncompressed (8 / 16 bit, with a linearization table) or lossless JPEG
// (ITU-T T.81 process 14, predictors 1-7, tiles or strips decoded in parallel); a 2x2
// Bayer CFA. Then: black / white levels, the camera's white balance (AsShotNeutral),
// demosaicing (Malvar-He-Cutler), the colour matrix (ColorMatrix for D65) to linear
// sRGB, BaselineExposure, the sRGB curve; the default crop. A plain development, like a
// raw converter's defaults without noise reduction or lens corrections (OpcodeLists).
// Not handled (returns an error): linear (demosaiced) DNGs, other CFA layouts, lossy
// JPEG / deflate compression, floating point data.
#ifndef NITRODNG_H
#define NITRODNG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;   // the developed image (the default crop), before rotation
    int orientation;     // EXIF-style 1..8
    float ev;            // exposure correction in stops: set by the caller after ndng_read_info (0: none)
    // internal
    int raw_w, raw_h;    // the raw IFD's image
    int area[4];         // active area: top, left, bottom, right
    int crop[4];         // default crop within it: x, y, w, h
    int compression, bps, cfa[4];   // cfa: colour (0 R, 1 G, 2 B) of the 2x2 positions
    int tile_w, tile_h, ntiles;      // (strips: tile_w = raw_w, tile_h = rows per strip)
    const uint8_t *tiles;            // the TIFF and its tables (offsets read when decoding)
    uint32_t tile_off_pos, tile_cnt_pos, tile_off_type, tile_cnt_type;
    int big_endian;
    float black[4], white;           // per 2x2 position
    uint32_t delta_h[3], delta_v[3]; // BlackLevelDeltaH / V (per column / row of the active area): type, count, pos
    float neutral[3];                // AsShotNeutral
    float cam_xyz[9];                // ColorMatrix (XYZ to camera) for D65
    float exposure;                  // BaselineExposure (stops)
    const uint8_t *lin;              // LinearizationTable (SHORTs) and its length
    int lin_n;
} ndng_info;

// Parses the TIFF / DNG structure. Returns 0 if nitrodng can develop the raw image.
int ndng_read_info(const uint8_t *data, size_t len, ndng_info *info);

// Develops it into 8-bit sRGB pixels, B G R X order (width x height, pitch bytes per row).
// nthreads 1: on the calling thread, else in parallel. Returns 0 on success.
int ndng_decode(const uint8_t *data, size_t len, const ndng_info *info, uint8_t *bgrx, size_t pitch, int nthreads);

#endif
