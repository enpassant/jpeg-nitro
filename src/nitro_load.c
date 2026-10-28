// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro_load: see nitro_load.h
#include "nitro_load.h"
#include "nitrojpeg.h"   // nj_find_embedded: the JPEG in a camera raw file

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define PAD 64   // zero bytes after the data read (the decoders may look a few bytes ahead)

// ---------------------------------------------------------------------------
// file names

const char *const nv_image_exts[] = {"jpg", "jpeg", "jpe", "jfif", "png", "heic", "heif", "avif", "webp", "tif", "tiff",
                                     "gif", "bmp", "psd", "psb", "cr2", "nef", "nrw", "arw", "raf", "dng",
#ifdef NV_SVG
                                     "svg",
#endif
#ifdef NV_CDR
                                     "cdr", "cmx",
#endif
                                     NULL};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// the extension (after the last dot of the last path component), NULL if there is none
static const char *extension(const char *path) {
    const char *name = path, *dot = NULL;
    for (const char *c = path; *c; c++) {
        if (*c == '/' || *c == '\\') name = c + 1;
    }
    for (const char *c = name; *c; c++)
        if (*c == '.') dot = c;
    return dot ? dot + 1 : NULL;
}

int nv_has_ext(const char *path, const char *const *exts) {
    const char *e = extension(path);
    if (!e) return 0;
    for (int i = 0; exts[i]; i++) {
        const char *a = e, *b = exts[i];
        while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
        if (!*a && !*b) return 1;
    }
    return 0;
}

int nv_is_image_name(const char *path) { return nv_has_ext(path, nv_image_exts); }

#ifdef _WIN32
int nv_is_image_name_w(const wchar_t *path) {
    const wchar_t *name = path, *dot = NULL;
    for (const wchar_t *c = path; *c; c++)
        if (*c == L'/' || *c == L'\\') name = c + 1;
    for (const wchar_t *c = name; *c; c++)
        if (*c == L'.') dot = c;
    if (!dot) return 0;
    for (int i = 0; nv_image_exts[i]; i++) {
        const wchar_t *a = dot + 1;
        const char *b = nv_image_exts[i];
        while (*a && *b && *a < 128 && lower((int)*a) == (int)*b) a++, b++;
        if (!*a && !*b) return 1;
    }
    return 0;
}
#endif

char *nv_ext_list(char *buf, size_t n, const char *prefix, const char *sep) {
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; nv_image_exts[i] && o < n; i++)
        o += (size_t)snprintf(buf + o, n - o, "%s%s%s", i ? sep : "", prefix, nv_image_exts[i]);
    buf[n - 1] = 0;
    return buf;
}

// ---------------------------------------------------------------------------
// reading

static FILE *open_rb(const char *path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    FILE *f = _wfopen(w, L"rb");
    free(w);
    return f;
#else
    return fopen(path, "rb");
#endif
}

static int seek_to(FILE *f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int64_t file_size(FILE *f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) return -1;
    int64_t n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END)) return -1;
    int64_t n = (int64_t)ftello(f);
#endif
    return seek_to(f, 0) ? -1 : n;
}

static int read_all(FILE *f, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = fread((uint8_t *)buf + got, 1, n - got, f);
        if (!r) return -1;
        got += r;
    }
    return 0;
}

static int pread_all(FILE *f, void *buf, size_t n, uint64_t off) {
    if (seek_to(f, off)) return -1;
    return read_all(f, buf, n);
}

uint8_t *nv_read_file(const char *path, size_t *len) {
    FILE *f = open_rb(path);
    if (!f) return NULL;
    int64_t sz = file_size(f);
    uint8_t *d = NULL;
    if (sz > 0 && (uint64_t)sz < ((uint64_t)1 << 40) && (d = malloc((size_t)sz + PAD))) {
        if (read_all(f, d, (size_t)sz)) {
            free(d);
            d = NULL;
        } else {
            memset(d + sz, 0, PAD);
            *len = (size_t)sz;
        }
    }
    fclose(f);
    return d;
}

uint8_t *nv_read_psd_merged(const char *path, size_t *len) {
    FILE *f = open_rb(path);
    if (!f) return NULL;
    uint8_t *out = NULL, h[26], b[8];
    int64_t sz = file_size(f);
    if (sz <= 0 || pread_all(f, h, 26, 0) || memcmp(h, "8BPS", 4)) goto done;
    {
        uint64_t size = (uint64_t)sz, pos = 26;
        int psb = h[5] == 2;
        size_t L = psb ? 8 : 4;
        for (int k = 0; k < 2; k++) {   // colour mode data, image resources
            if (pread_all(f, b, 4, pos)) goto done;
            pos += 4 + ((uint64_t)b[0] << 24 | (uint64_t)b[1] << 16 | (uint64_t)b[2] << 8 | b[3]);
            if (pos > size) goto done;
        }
        uint64_t head = pos, lm = 0;   // bytes kept as they are
        if (pread_all(f, b, L, pos)) goto done;
        for (size_t i = 0; i < L; i++) lm = lm << 8 | b[i];
        uint64_t x = pos + L + lm;   // merged image data (compression field first)
        if (lm > size || x + 2 > size) goto done;
        int16_t count = 0;
        if (lm >= L + 2) {
            uint8_t c[10];
            if (pread_all(f, c, L + 2, pos + L)) goto done;
            uint64_t li = 0;
            for (size_t i = 0; i < L; i++) li = li << 8 | c[i];
            if (li >= 2) count = (int16_t)(c[L] << 8 | c[L + 1]);
        }
        uint64_t merged = size - x, total = head + L + L + 2 + merged;
        out = malloc((size_t)total + PAD);
        if (!out) goto done;
        if (pread_all(f, out, (size_t)head, 0)) { free(out); out = NULL; goto done; }
        uint8_t *q = out + head;
        uint64_t slim = L + 2;   // layer and mask section: layer info (length 2) + layer count
        for (size_t i = 0; i < L; i++) q[i] = (uint8_t)(slim >> (8 * (L - 1 - i)));
        for (size_t i = 0; i < L; i++) q[L + i] = (uint8_t)(2 >> (8 * (L - 1 - i)));
        q[2 * L] = (uint8_t)((uint16_t)count >> 8);
        q[2 * L + 1] = (uint8_t)count;
        if (pread_all(f, q + 2 * L + 2, (size_t)merged, x)) { free(out); out = NULL; goto done; }
        memset(out + total, 0, PAD);
        *len = (size_t)total;
    }
done:
    fclose(f);
    return out;
}

uint8_t *nv_read_raw_jpeg(const char *path, size_t *len, int *orientation) {
    static const char *const e[] = {"cr2", "nef", "nrw", "arw", "raf", NULL};
    if (!nv_has_ext(path, e)) return NULL;
    FILE *f = open_rb(path);
    if (!f) return NULL;
    uint8_t *out = NULL;
    int64_t sz = file_size(f);
    size_t cap = 1 << 20;   // (Nikon puts the SubIFD directories after the maker notes: ~270 KB in)
    uint8_t *hdr = malloc(cap);
    size_t n = hdr ? fread(hdr, 1, cap, f) : 0, off, size;
    if (n > 0 && sz > 0 && !nj_find_embedded(hdr, n, &off, &size, orientation) && off < (uint64_t)sz &&
        size <= (uint64_t)sz - off && size < ((size_t)1 << 31) && (out = malloc(size + PAD))) {
        if (!pread_all(f, out, size, off)) {
            memset(out + size, 0, PAD);
            *len = size;
        } else {
            free(out);
            out = NULL;
        }
    }
    free(hdr);
    fclose(f);
    return out;
}
