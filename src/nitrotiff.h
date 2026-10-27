// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrotiff: decodes the image of a TIFF file (its first full-size one) into 8-bit BGRX,
// written from the TIFF 6.0 specification, without dependencies. Strips and tiles are decoded in
// parallel, an uncompressed image is converted straight from the file, in parallel too.
//
// Compression: none, PackBits, LZW, Deflate (through nitropng's inflate), each with the
// horizontal predictor; CCITT G3 (MH, T.4 1D / 2D) and G4 (T.6) for bilevel images.
// Samples: 1 / 2 / 4 / 8 / 16-bit gray (black or white is zero) and palette (up to 8 bits),
// 8 / 16-bit RGB and CMYK (naive conversion, no profile); an alpha channel (associated or not)
// is applied over black; chunky or planar. The 16-bit samples are rounded to 8 bits.
// Not handled (returns an error: the viewers use the system's decoder): JPEG compression,
// YCbCr, floating point and 32-bit samples, BigTIFF.
#ifndef NITROTIFF_H
#define NITROTIFF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;
    int orientation;     // EXIF-style 1..8
    const uint8_t *icc;  // its ICC profile (InterColorProfile) in the file, or NULL
    size_t icc_len;
    // internal
    int le;              // little-endian ("II")
    int bps, spp, color, alpha;   // bits per sample, samples per pixel, colour samples, 1 associated / 2 unassociated alpha
    int photometric, compression, predictor, planar, fillorder, t4options;
    int tiled, cw, ch;   // chunk (strip / tile) size; strips: cw = width, ch = rows per strip
    int across, down, nchunks;     // chunks per plane: across x down; all: x samples when planar
    uint32_t off_pos, off_type, cnt_pos, cnt_type;   // StripOffsets / TileOffsets and the byte counts
    uint32_t cmap_pos;   // ColorMap: 3 x 2^bps SHORTs
} nt_info;

// Parses the TIFF structure. Returns 0 if nitrotiff can decode the image.
int nt_read_info(const uint8_t *data, size_t len, nt_info *info);

// Decodes it into B G R X pixels (width x height, pitch bytes per row; transparent areas black).
// nthreads 1: on the calling thread, else in parallel. Returns 0 on success.
int nt_decode(const uint8_t *data, size_t len, const nt_info *info, uint8_t *bgrx, size_t pitch, int nthreads);

// The size of the first image from its directory alone (for a window size before the whole file is
// read; the directory of a Photoshop TIFF is at its end): dir holds the n bytes at the directory's
// offset, le as in the header. Returns 0 if it found them.
int nt_dir_size(const uint8_t *dir, size_t n, int le, int *width, int *height, int *orientation);

#endif
