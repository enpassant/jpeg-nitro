// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro-convert: decoded planes -> RGB pixels for the GNOME viewer. Multi-threaded
// (src/nitro_os.h), plain C without GTK, so it can be tested on its own.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../nitro_load.h"

#ifdef __cplusplus
extern "C" {
#endif

// An NV_YUV image (nitro_load.h: a JPEG's planes from nj_decode_planes, any sampling; HEIC's and
// WebP's 4:2:0) in buf -> RGB, 3 bytes per pixel, rows 'stride' bytes apart. Chroma upsampling
// and YCbCr->RGB as libjpeg(-turbo) does it ("fancy" upsampling for 4:2:2, 4:2:0 and 4:4:0,
// replication for other factors, the same integer arithmetic): the output is identical to
// libjpeg-turbo's RGB output. Limited range (img->limited: WebP) is scaled to full, BT.601.
void nc_yuv_to_rgb(const NvImage *img, const uint8_t *buf, uint8_t *dst, size_t stride);

// An NV_PLANAR image (a PSD's planes from ps_decode) in buf -> RGB. Merged transparency
// (Photoshop stores the colour matted with white) is shown over black, as the macOS and
// Windows viewers do.
void nc_psd_to_rgb(const NvImage *img, const uint8_t *buf, uint8_t *dst, size_t stride);

#ifdef __cplusplus
}
#endif
