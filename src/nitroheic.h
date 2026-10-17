// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroheic (macOS): HEIC photos (iPhone and others) decoded tile by tile in parallel.
//
// An iPhone photo is a grid of 512 x 512 tiles, each an independent HEVC still picture.
// ImageIO decodes them one after the other (~200 ms for 12 MP on an Intel i9, ~59 ms on an
// M1); here every worker has its own VideoToolbox session (Intel: software decoders, one
// per core up to 8; Apple Silicon: 4 on the media engine) and takes the next tile, and the
// result is planar Y/Cb/Cr like nitrojpeg's (colour conversion on the GPU): ~33 ms on the
// i9, ~26 ms on the M1. Only the HEIF structure is parsed here; HEVC decoding is Apple's.
//
// Handled: an 8-bit 4:2:0 HEVC image or grid of them (full range BT.601, as iPhones write
// them), with rotation (irot), mirroring (imir), a crop from the top left corner (clap)
// and an ICC profile. Not handled (returns an error: use ImageIO): alpha, 10-bit, other
// chroma formats, BT.709 or limited range YCbCr, other crops, damaged files.
#ifndef NITROHEIC_H
#define NITROHEIC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;          // visible size (before rotation)
    int cols, rows;             // tile grid (1 x 1: a single image)
    int tile_w, tile_h;
    int plane_w[3], plane_h[3]; // Y, Cb, Cr: the whole tile grid (cols * tile_w ...)
    int orientation;            // EXIF-style 1..8 from irot / imir
    const uint8_t *icc;         // ICC profile in the file (NULL: none), and its size
    size_t icc_len;
    // internal
    int ntiles;
    uint32_t tile_id[256];
    size_t tile_off[256], tile_len[256];
    const uint8_t *hvcc;        // HEVC decoder configuration of the tiles
    size_t hvcc_len;
} nh_info;

// Parses the HEIF structure. Returns 0 if nitroheic can decode the primary image.
int nh_read_info(const uint8_t *data, size_t len, nh_info *info);

// Decodes into planar Y/Cb/Cr (full range, BT.601, as JPEG). planes[c] must hold
// plane_h[c] rows of pitch[c] bytes. nthreads: parallel decoders (<= 0: the CPUs, at most 8;
// Apple Silicon: 4).
// Returns 0 on success.
int nh_decode_planes(const uint8_t *data, size_t len, const nh_info *info, uint8_t *const planes[3],
                     const size_t pitch[3], int nthreads);

#endif
