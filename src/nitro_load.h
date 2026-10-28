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

// What a viewer decodes: a PSD's merged image, a camera raw's JPEG (*raw_read = 1, *raw_orientation:
// the raw file's, 0: the JPEG's own) or the whole file; with the 64-byte padding. A PSD nitropsd
// cannot decode, or larger than max_tex, is read whole (the platform's decoder gets it). NULL if
// unreadable.
uint8_t *nv_read_image(const char *path, size_t *len, int max_tex, int *raw_orientation, int *raw_read);

// ---------------------------------------------------------------------------
// decoding with our decoders

typedef enum { NV_YUV = 0, NV_BGRA = 1, NV_PNG = 2, NV_PLANAR = 3 } NvKind;   // what the buffer holds (see NvImage)

// NvImage.mode: 0..3 a baseline JPEG's nitrojpeg mode (single / RST / split / progressive), the
// decoder otherwise; the viewers' own fallback decoders use -1 .. -3.
enum { NV_MODE_NITROPNG = -4, NV_MODE_NITROPSD = -5, NV_MODE_SVG = -6, NV_MODE_CDR = -7, NV_MODE_NITROHEIC = -8,
       NV_MODE_NITRODNG = -9, NV_MODE_NITROWEBP = -10, NV_MODE_NITROTIFF = -11, NV_MODE_NITROAVIF = -12 };
const char *nv_mode_name(int mode);   // of the modes above; NULL for others

// A decoded image in a host buffer (what the GPU shaders or a CPU conversion read).
typedef struct {
    NvKind kind;                // YUV: planes (JPEG: any sampling; HEIC / WebP: 4:2:0); BGRA: 8-bit pixels (X 255
                                // where there is no alpha); PNG: the unfiltered rows, each after its filter-type
                                // byte (pitch[0] = stride + 1); PLANAR: a PSD's 8-bit planes, width x height each,
                                // one after the other
    int width, height, ncomp;   // ncomp: YUV components / PNG channels / PSD colour planes
    int orientation;            // EXIF 1..8 (also known for a JPEG our decoder could not decode)
    int h[3], v[3], hmax, vmax; // YUV: the components' sampling factors
    int hasa;                   // PLANAR: a transparency plane after the colour planes
    int limited;                // YUV: limited range (Y 16..235, as WebP), not full range
    int pw[3], ph[3];           // YUV: the planes' sizes
    size_t offset[3], pitch[3]; // the planes in the buffer; BGRA / PNG: pitch[0]
    size_t size;                // bytes of image data in the buffer
    int mode, bands;            // the decoder (see above), the parallel pieces it worked in
    int ev3;                    // a DNG: the exposure correction it was developed with (1/3 EV)
    const uint8_t *icc;         // the file's ICC profile when the decoder read one (TIFF: RGB images only;
    size_t icc_len;             //   HEIC, AVIF, WebP): into the file data, valid as long as that is
    const char *error;          // not decoded by us: why, when a decoder recognised the file (static text)
} NvImage;

// How nv_decode gets buffers and how it decodes.
typedef struct {
    uint8_t *(*get)(size_t len, size_t *cap, void *ctx);   // a buffer of at least len bytes (*cap: its size), NULL if none
    void (*put)(uint8_t *p, size_t cap, void *ctx);         // one back
    void *ctx;
    size_t align;      // plane pitches and offsets (and a BGRA pitch) rounded up to this (>= 1)
    int max_tex;       // images wider or taller than this are left to the platform's decoder
    int threads;       // decoder threads (0: all)
    int dng_ev3;       // DNG exposure correction, 1/3 EV
} NvDecoder;

// Decodes data (as nv_read_image read it; a camera raw's or DNG's embedded JPEG is moved to its
// start, *len shortened) with our decoders: DNG, JPEG, PNG, PSD, HEIC, AVIF (NV_DAV1D), WebP, TIFF.
// Returns 0: decoded, *img describes the pixels in *buf (from dec->get, *cap its size); 1: not
// one of ours, or a file they cannot decode (img->error says which; img->orientation may be known
// already): the platform's decoder gets it; -1: out of memory.
int nv_decode(uint8_t *data, size_t *len, int raw_orientation, int raw_read, const NvDecoder *dec, NvImage *img,
              uint8_t **buf, size_t *cap);

// The image's size (after its EXIF orientation) from the file's header (its first MB), without
// decoding: for the first window's size. 0 if it is not known (the platform's decoder may tell).
int nv_header_size(const char *path, double *w, double *h);

#ifdef __cplusplus
}
#endif
