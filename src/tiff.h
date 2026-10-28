// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// tiff.h (internal): reading TIFF directories - shared by nitrotiff and nitrodng (TIFF / DNG
// files), nitrojpeg (camera raw files, EXIF), nitropng (eXIf) and nitrowebp (EXIF). Header-only,
// static functions; every read is bounds-checked (0 beyond the data).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *d;
    size_t len;
    int le;   // little-endian ("II")
} Tiff;

static inline uint32_t t16(const Tiff *t, size_t o) {
    if (o + 2 > t->len) return 0;
    const uint8_t *p = t->d + o;
    return t->le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : (uint32_t)p[0] << 8 | p[1];
}

static inline uint32_t t32(const Tiff *t, size_t o) {
    if (o + 4 > t->len) return 0;
    return t->le ? t16(t, o) | t16(t, o + 2) << 16 : t16(t, o) << 16 | t16(t, o + 2);
}

typedef struct {
    uint32_t type, count, pos;   // pos: where the value(s) are
    int found;                   // (and they are within the data)
} Entry;

static const int tiff_type_size[14] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};   // (13: IFD, an offset)

// The entry of tag `want` in the directory at ifd: found = 0 if it is not there, has an unknown
// type, an absurd count, or its values lie beyond the data.
static inline Entry tag(const Tiff *t, uint32_t ifd, unsigned want) {
    Entry e = {0, 0, 0, 0};
    if ((size_t)ifd + 2 > t->len) return e;
    uint32_t n = t16(t, ifd);
    for (uint32_t i = 0; i < n; i++) {
        size_t p = ifd + 2 + 12 * (size_t)i;
        if (p + 12 > t->len) break;
        if (t16(t, p) != want) continue;
        e.type = t16(t, p + 2);
        e.count = t32(t, p + 4);
        uint64_t sz = e.type < 14 ? (uint64_t)tiff_type_size[e.type] * e.count : 0;
        if (!sz || e.count > (1u << 28)) return e;
        e.pos = sz <= 4 ? (uint32_t)(p + 8) : t32(t, p + 8);   // (inline, or an offset)
        e.found = e.pos < t->len && sz <= t->len - e.pos;
        return e;
    }
    return e;
}

// the k-th value of an entry: BYTE / SHORT / LONG / IFD (0 for the other types)
static inline uint32_t tiff_u32(const Tiff *t, Entry e, uint32_t k) {
    if (!e.found || k >= e.count) return 0;
    switch (e.type) {
    case 1: case 7: return t->d[e.pos + k];
    case 3: return t16(t, e.pos + 2 * (size_t)k);
    case 4: case 13: return t32(t, e.pos + 4 * (size_t)k);
    default: return 0;
    }
}

// the k-th value of an entry as a number: integers, rationals and floats
static inline double tiff_val(const Tiff *t, Entry e, uint32_t k) {
    if (!e.found || k >= e.count) return 0;
    switch (e.type) {
    case 1: case 7: return t->d[e.pos + k];
    case 6: return (int8_t)t->d[e.pos + k];
    case 3: return t16(t, e.pos + 2 * (size_t)k);
    case 8: return (int16_t)t16(t, e.pos + 2 * (size_t)k);
    case 4: case 13: return t32(t, e.pos + 4 * (size_t)k);
    case 9: return (int32_t)t32(t, e.pos + 4 * (size_t)k);
    case 5: {
        uint32_t den = t32(t, e.pos + 8 * (size_t)k + 4);
        return den ? (double)t32(t, e.pos + 8 * (size_t)k) / den : 0;
    }
    case 10: {
        int32_t den = (int32_t)t32(t, e.pos + 8 * (size_t)k + 4);
        return den ? (double)(int32_t)t32(t, e.pos + 8 * (size_t)k) / den : 0;
    }
    case 11: {
        uint32_t u = t32(t, e.pos + 4 * (size_t)k);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    default: return 0;
    }
}

// the directory after ifd (0: none, or beyond the data)
static inline uint32_t tiff_next_ifd(const Tiff *t, uint32_t ifd) { return t32(t, ifd + 2 + 12 * (size_t)t16(t, ifd)); }

// The Orientation tag (0x112) of an Exif block (a TIFF structure, with or without the "Exif\0\0"
// header of a JPEG's APP1 segment / a WebP's EXIF chunk): 1..8, 1 if none.
static inline int tiff_orientation(const uint8_t *p, size_t n) {
    if (n >= 6 && !memcmp(p, "Exif\0\0", 6)) {
        p += 6;
        n -= 6;
    }
    if (n < 8 || p[0] != p[1] || (p[0] != 'I' && p[0] != 'M')) return 1;
    Tiff t = {p, n, p[0] == 'I'};
    if (t16(&t, 2) != 42) return 1;
    uint32_t o = tiff_u32(&t, tag(&t, t32(&t, 4), 0x112), 0);
    return o >= 1 && o <= 8 ? (int)o : 1;
}
