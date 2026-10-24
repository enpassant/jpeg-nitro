// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro-convert: decoded planes -> RGB pixels for the GNOME viewer. Multi-threaded
// (src/nitro_os.h), plain C without GTK, so it can be tested on its own.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../nitrojpeg.h"
#include "../nitropsd.h"

#ifdef __cplusplus
extern "C" {
#endif

// JPEG planes from nj_decode_planes -> RGB, 3 bytes per pixel, rows 'stride' bytes apart.
// Chroma upsampling and YCbCr->RGB as libjpeg(-turbo) does it ("fancy" upsampling for
// 4:2:2, 4:2:0 and 4:4:0, replication for other factors, the same integer arithmetic):
// the output is identical to libjpeg-turbo's RGB output.
void nc_jpeg_to_rgb(const nj_info *fi, uint8_t *const planes[3], const size_t pitch[3], uint8_t *dst, size_t stride);

// 4:2:0 planes (HEIC: full range, limited 0; WebP: limited range, limited 1) -> RGB, BT.601,
// upsampled as above.
void nc_yuv420_to_rgb(int width, int height, uint8_t *const planes[3], const size_t pitch[3], int limited, uint8_t *dst,
                      size_t stride);

// PSD planes from ps_decode -> RGB. Merged transparency (Photoshop stores the colour matted
// with white) is shown over black, as the macOS and Windows viewers do.
void nc_psd_to_rgb(const ps_info *si, const uint8_t *planes, uint8_t *dst, size_t stride);

#ifdef __cplusplus
}
#endif
