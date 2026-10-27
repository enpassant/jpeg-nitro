// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroavif: AVIF images (AV1 stills in a HEIF container) into 8-bit BGRX. The HEIF structure is
// read here (src/heif.h, as for HEIC); AV1 is decoded by dav1d (VideoLAN, BSD-2-Clause,
// third_party/dav1d from scripts/get-deps.sh); the colour conversion is ours, in parallel.
//
// Handled: a single image or a grid of tiles (decoded in parallel), 8 / 10 / 12 bits, 4:2:0 /
// 4:2:2 / 4:4:4 / monochrome, BT.601 / BT.709 / BT.2020 / identity (RGB) matrices, full or
// limited range (from the nclx colour box, else from the AV1 sequence header), an alpha plane
// (applied over black), rotation (irot), mirroring (imir), a crop (clap, whole pixels), layered
// images (the operating point of a1op; their data in several pieces), an ICC profile (returned, not
// applied). Chroma is upsampled linearly (centred, as JPEG's); limited range is expanded (gray too).
// Not handled (returns an error): YCgCo, damaged files; HDR transfer functions (PQ, HLG) are shown
// as they are.
#ifndef NITROAVIF_H
#define NITROAVIF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;          // visible size (before rotation)
    int orientation;            // EXIF-style 1..8 from irot / imir
    int alpha;                  // has an alpha plane (0 also when the colours are premultiplied by it)
    const uint8_t *icc;         // ICC profile in the file (NULL: none), and its size
    size_t icc_len;
    // internal
    int cols, rows, ntiles;     // tile grid (1 x 1: a single image)
    int tile_w, tile_h;
    size_t tile_off[256], tile_len[256];    // the AV1 data of the colour tiles
    size_t alpha_off[256], alpha_len[256];  // ... and of the alpha tiles (the same grid)
    int cpieces, apieces;       // a single image in several pieces (layered): its colour / alpha data
    size_t cpiece_off[8], cpiece_len[8], apiece_off[8], apiece_len[8];
    int crop_x, crop_y;         // clap: the visible part's top left corner in the (grid) image
    int op;                     // AV1 operating point (a1op)
    int nclx, matrix, full;     // colour box nclx: matrix coefficients, full range
} na_info;

// Parses the HEIF structure. Returns 0 if nitroavif can decode the primary image.
int na_read_info(const uint8_t *data, size_t len, na_info *info);

// Decodes it into B G R X pixels (width x height, pitch bytes per row; transparent areas black).
// nthreads: 1 on the calling thread, <= 0 all CPUs. Returns 0 on success.
int na_decode(const uint8_t *data, size_t len, const na_info *info, uint8_t *bgrx, size_t pitch, int nthreads);

#endif
