// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrowebp: lossy WebP images (a VP8 key frame) decoded into planar Y/Cb/Cr, written from the
// VP8 specification (RFC 6386) and the WebP container format, without dependencies.
//
// The coefficient tokens are one serial stream (WebP encoders write a single token partition):
// they are parsed on one thread while another parses the modes (a stream of their own) and a
// third reconstructs the rows behind them, so a picture takes about as long as its tokens.
//
// Work in progress: no loop filter (the picture is decoded before it; intra prediction uses the
// unfiltered pixels, so leaving it out changes only the block edges). Not handled (returns an
// error: use another decoder): lossless WebP (VP8L), alpha, animation.
#ifndef NITROWEBP_H
#define NITROWEBP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;          // image size
    int plane_w[3], plane_h[3]; // Y, Cb, Cr (chroma: half size, rounded up)
    int orientation;            // EXIF-style 1..8 (EXIF chunk; 1 if none)
    const uint8_t *icc;         // ICC profile in the file (NULL: none), and its size
    size_t icc_len;
    // internal
    const uint8_t *vp8;         // the VP8 frame
    size_t vp8_len;
} nw_info;

// Parses the WebP container. Returns 0 if nitrowebp can decode the image.
int nw_read_info(const uint8_t *data, size_t len, nw_info *info);

// Decodes into planar Y/Cb/Cr (limited range BT.601, as VP8 defines it). planes[c] must hold
// plane_h[c] rows of pitch[c] bytes. Returns 0 on success.
int nw_decode_planes(const uint8_t *data, size_t len, const nw_info *info, uint8_t *const planes[3],
                     const size_t pitch[3], int nthreads);

#endif
