// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_load: what the viewers (nitroview.m, nitroview_sdl.c, nitroview_win.c, src/gnome) share
// of loading an image: which files are images, reading them (a PSD's merged image only, a camera
// raw's embedded JPEG only). Plain C, no platform code: paths are UTF-8 (on Windows they are
// converted for the file API).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The file extensions of the images a viewer opens from a folder: our decoders' formats and
// what every platform's fallback decoder reads (NV_SVG / NV_CDR add svg / cdr, cmx).
// NULL-terminated, lower case, without the dot.
extern const char *const nv_image_exts[];

// Is path's extension (case-insensitively) one of exts (NULL-terminated, without the dot)?
int nv_has_ext(const char *path, const char *const *exts);
// ... one of nv_image_exts?
int nv_is_image_name(const char *path);
#ifdef _WIN32
int nv_is_image_name_w(const wchar_t *path);
#endif
// nv_image_exts as one string, each with prefix before it and sep between them (a file dialog's
// filter: "jpg;jpeg;..." or "*.jpg;*.jpeg;..."). Returns buf.
char *nv_ext_list(char *buf, size_t n, const char *prefix, const char *sep);

// The whole file, followed by 64 zero bytes (the decoders may look a little ahead). NULL if it
// cannot be read (or is empty / absurdly large).
uint8_t *nv_read_file(const char *path, size_t *len);

// PSD / PSB: only what the merged image needs (most of a big PSD is layer data). A slim PSD in
// memory: header + colour mode data + image resources (ICC profile) + an empty layer section
// that keeps the layer count's sign (merged transparency) + the merged image data; the 64-byte
// padding after it. NULL if the file is not a sane PSD (the caller reads it all).
uint8_t *nv_read_psd_merged(const char *path, size_t *len);

// Camera raw files by their extension (CR2, NEF, NRW, ARW, RAF): reads only the start (the TIFF
// directories) and the full-size JPEG nitrojpeg finds there, not the raw data (most of the 10-30
// MB); with the 64-byte padding. *orientation: the raw file's (0: the JPEG's own, RAF). NULL if
// it is not such a file or there is no such JPEG (the caller reads it all).
uint8_t *nv_read_raw_jpeg(const char *path, size_t *len, int *orientation);

#ifdef __cplusplus
}
#endif
