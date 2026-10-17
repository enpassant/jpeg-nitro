// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrohevc: a small HEVC (H.265) decoder for still pictures, written from the
// specification (ITU-T H.265), without dependencies. It decodes the intra pictures
// HEIC photos are made of (one picture per tile of an iPhone photo): Main / Main
// Still Picture profile, 8-bit 4:2:0, I slices.
//
// Work in progress: no deblocking filter and no SAO yet (the picture is decoded
// before the in-loop filters); not handled (returns an error): PCM blocks, tiles inside
// a picture, dependent slices, other bit depths and chroma formats.
#ifndef NITROHEVC_H
#define NITROHEVC_H

#include <stddef.h>
#include <stdint.h>

// Decodes one picture: the parameter sets come from an hvcC box (HEIF / MP4 decoder
// configuration), the picture's NAL units from data (length-prefixed, as in HEIF).
// planes[0..2] receive Y, Cb, Cr of width x height (the picture after its conformance
// window; chroma half size), full range BT.601 (as JPEG): a picture whose VUI says
// otherwise (BT.709, limited range) returns an error. threads: a picture coded with WPP in
// one slice has its CTB rows decoded in parallel by that many threads (<= 0: the CPUs; 1: none,
// e.g. when the caller decodes several pictures in parallel). Returns 0 on success.
int nhevc_decode(const uint8_t *hvcc, size_t hvcc_len, const uint8_t *data, size_t len, int width, int height,
                 uint8_t *const planes[3], const size_t pitch[3], int threads);

#endif
