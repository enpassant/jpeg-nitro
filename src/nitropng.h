// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitropng: multi-threaded decoding of a single PNG image, no dependencies.
//
// PNG = deflate (LZ77 + Huffman) + per-row filters. Both are sequential by
// design; nitropng decodes them in parallel anyway:
//   * inflate: the compressed stream is cut into chunks; each thread finds the
//     next deflate block header in its chunk and decodes from there. Back-
//     references into the (still unknown) previous 32 KB are written as
//     placeholders, resolved once the previous chunk is known. The result is
//     verified with the stream's Adler-32.
//   * filters: rows are reversed as a "wavefront": row r trails row r-1.
//
// Output: the unfiltered rows exactly as in the file, each still preceded by
// its filter-type byte: row y starts at out + y * (stride + 1) + 1. For 8-bit
// gray / gray+alpha / RGB / RGBA (supported = 1) that is ready for a GPU; palette,
// 1 / 2 / 4 / 16-bit images and transparency keys (convertible = 1) become 8-bit
// BGRX through np_to_bgrx. Interlaced (Adam7) images are not handled.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width, height;
    int bit_depth, color_type, interlace;
    int channels;          // 1 gray / palette, 2 gray+alpha, 3 RGB, 4 RGBA
    int bpp;               // bytes per pixel for the filters (at least 1)
    size_t stride;         // bytes per row, without the filter byte
    size_t raw_size;       // height * (stride + 1): size of the decoded buffer
    int has_icc;           // iCCP chunk present (colour management is up to the caller)
    int orientation;       // EXIF orientation 1..8 from an eXIf chunk (1: none); applying it is up to the caller
    int supported;         // 1: 8-bit gray / gray+alpha / RGB / RGBA, non-interlaced: the rows as they are
    int convertible;       // 1: np_decode + np_to_bgrx can handle it (also palette, 1 / 2 / 4 / 16-bit, tRNS)
    uint32_t plte_pos, plte_n, trns_pos, trns_n;   // PLTE / tRNS chunk data in the file (0: none)
} np_info;

typedef struct {
    double parse_ms, inflate_ms, unfilter_ms;
    int chunks;            // parallel inflate chunks used (1 = sequential)
    int mode;              // 0 sequential, 1 parallel
} np_stats;

// Parses the header. Returns 0 if the file is a PNG we could read at all.
int np_read_info(const uint8_t *data, size_t len, np_info *info);

// Decodes into out (info->raw_size bytes, see above). nthreads <= 0: all CPUs,
// 1: single-threaded. Returns 0 on success (checksum verified), -1 otherwise.
int np_decode(const uint8_t *data, size_t len, const np_info *info, uint8_t *out, int nthreads, np_stats *stats);

// The decoded rows (np_decode's out) as 8-bit B G R X, transparency applied over black, 16-bit
// samples rounded (width x height, pitch bytes per row; in parallel unless nthreads == 1).
// data: the file (palette, transparency key). Returns 0 on success.
int np_to_bgrx(const uint8_t *data, size_t len, const np_info *info, const uint8_t *raw, uint8_t *bgrx, size_t pitch,
               int nthreads);

// Decompresses a complete zlib stream of known output size (parallel unless
// nthreads == 1), verifying its Adler-32. Returns 0 on success.
int np_zlib_decompress(const uint8_t *z, size_t zlen, uint8_t *out, size_t outlen, int nthreads);

// Testing / tuning: number of parallel inflate chunks (0 = 2 x threads).
void np_set_chunks(int n);

#ifdef __cplusplus
}
#endif
