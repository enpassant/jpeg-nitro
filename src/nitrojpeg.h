// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrojpeg: multi-threaded decoding of a *single* baseline or progressive JPEG image into
// planar Y/Cb/Cr, with no external dependencies (C, pthreads, AVX2 / NEON).
//
// A baseline JPEG is one long Huffman-coded bitstream, so normal decoders
// (libjpeg-turbo included) run on one core. nitrojpeg decodes it in parallel:
//   * files without restart markers: the stream is cut into many chunks and
//     every thread starts decoding at an arbitrary byte offset. Huffman codes
//     self-synchronise after a few MCUs; a stitching pass (in order) finds
//     where each chunk meets the true decoding path, which gives its MCU
//     index and DC offset. Coefficients are decoded exactly once and the
//     IDCT runs as soon as a chunk is stitched, while it is still in cache;
//   * files with restart markers: intervals are decoded in parallel;
//   * progressive files: the scans run as a pipeline, each a few MCU rows behind
//     the scans it depends on, the IDCT behind the last ones.
// The IDCT is libjpeg's ISLOW algorithm, so the output is bit-identical to it.
//
// Not handled (returns an error, use another decoder): arithmetic, 12-bit,
// lossless, CMYK/RGB, multi-scan baseline, damaged or cut-off streams.
// Progressive files without their last scans (previews) are decoded as they
// are, without libjpeg's block smoothing.
// Built with -DNJ_REFERENCE it also contains the old libjpeg-turbo based
// engine and BGRX output, for comparison and testing.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width, height;
    int ncomp;                  // 1 (gray) or 3 (YCbCr)
    int h[3], v[3];             // sampling factors (frame component order)
    int hmax, vmax;
    int mcus_per_row, mcu_rows; // MCU grid
    int plane_w[3], plane_h[3]; // padded plane size in samples (raw output)
    int restart_interval;       // DRI, 0 if none
    int orientation;            // EXIF orientation 1..8
    int supported;              // 1: nj_decode_* can handle it (else use a fallback)
} nj_info;

typedef struct {
    double scan_ms;   // marker scan + unstuffing
    double sync_ms;   // speculative Huffman decode + stitching
    double decode_ms; // parallel band decode
    int bands;
    int mode;         // 0 single, 1 restart markers, 2 speculative split, 3 progressive
    // speculative split only: for pieces 1..nsync, how far each thread had
    // to decode before it met the true decoding path (piece 0 starts right).
    // sync_mcus[i] = -1: never synchronised (the true path ran through it).
    int nsync;
    int32_t sync_mcus[256];   // "garbage" MCUs decoded before synchronising
    uint32_t sync_bits[256];  // distance of the sync point from the piece start
} nj_stats;

// Parses the headers. Returns 0 if the file is a JPEG we could read at all.
int nj_read_info(const uint8_t *data, size_t len, nj_info *info);

// Camera raw files (TIFF-based: Canon CR2, Nikon NEF, Sony ARW and others whose first
// image is a reduced one; Fujifilm RAF): the big JPEG the camera stored beside the raw
// data (CR2: the first image; RAF: the one its header points to; others: the biggest
// JPEG of the directories and SubIFDs). Returns 0 and its byte range and the EXIF
// orientation (1..8) of the raw file (the JPEG itself has none; RAF: 0, its JPEG has
// its own EXIF). It is sRGB (also
// when the camera is set to Adobe RGB). data may be just the start of the file
// (for the header): the range can then run past len.
int nj_find_embedded(const uint8_t *data, size_t len, size_t *offset, size_t *size, int *orientation);

// Decodes into planar Y/Cb/Cr (no upsampling, no colour conversion: meant
// for a GPU shader). planes[c] must hold plane_h[c] rows of pitch[c] bytes,
// pitch[c] >= plane_w[c]. nthreads <= 0 means all CPUs. Returns 0 on success.
int nj_decode_planes(const uint8_t *data, size_t len, const nj_info *info,
                     uint8_t *const planes[3], const size_t pitch[3],
                     int nthreads, nj_stats *stats);


// Limits the number of worker threads (0 = all logical CPUs; progressive files:
// 4, more can't be kept busy). The number of work pieces is the 'nthreads'
// argument of the decode functions (<= 0: 4 x logical CPUs).
void nj_set_max_workers(int n);
// Number of parallel tasks for restart-marker files, and bands of the
// reference engine (0: 2 x pieces).
void nj_set_bands(int n);
void nj_set_scalar_idct(int on);   // testing: plain C IDCT instead of AVX2 / NEON
void nj_set_chunks(int n);         // own engine: speculative chunks (0 = 32 x CPUs)
// The decoders keep their work buffers for the next image (they are as large as the largest image
// so far); this frees them.
void nj_free_buffers(void);

#ifdef NJ_REFERENCE
// ---- reference / comparison code, only when built with -DNJ_REFERENCE
// ---- (needs libjpeg-turbo). The viewer does not use any of it.

// Decodes into 32-bit BGRX (upsampled + converted on the CPU by libjpeg-turbo).
int nj_decode_bgrx(const uint8_t *data, size_t len, const nj_info *info,
                   uint8_t *dst, size_t pitch, int nthreads, nj_stats *stats);
// Planar engine: 0 (default) = own engine; 1 = old engine where libjpeg-turbo
// decodes the bands (Huffman decoded twice).
void nj_set_engine(int e);
// Old engine experiment: one sequential Huffman pass instead of speculation.
void nj_set_sequential_scan(int on);
#endif

#ifdef __cplusplus
}
#endif
