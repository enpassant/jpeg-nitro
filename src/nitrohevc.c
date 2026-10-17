// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrohevc: see nitrohevc.h. Section numbers refer to ITU-T H.265 (v4 and later).
#include "nitrohevc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLIP3(lo, hi, v) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static inline uint8_t clip_pixel(int v) { return (uint8_t)CLIP3(0, 255, v); }

// ---------------------------------------------------------------------------
// NAL units: emulation prevention bytes removed (RBSP), with the positions of the
// removed bytes kept (entry points count the bytes of the NAL unit itself)

typedef struct {
    uint8_t *data;     // RBSP
    size_t len;
    size_t *removed;   // positions (in the NAL unit) of the removed 0x03 bytes
    size_t nremoved;
} Rbsp;

static int unescape(const uint8_t *nal, size_t len, Rbsp *r) {
    r->data = malloc(len + 8);
    r->removed = malloc(sizeof(size_t) * (len / 3 + 1));
    if (!r->data || !r->removed) return -1;
    size_t o = 0, zeros = 0;
    r->nremoved = 0;
    for (size_t i = 0; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            r->removed[r->nremoved++] = i;
            zeros = 0;
            continue;
        }
        r->data[o++] = nal[i];
        zeros = nal[i] ? 0 : zeros + 1;
    }
    memset(r->data + o, 0, 8);
    r->len = o;
    return 0;
}

static void rbsp_free(Rbsp *r) {
    free(r->data);
    free(r->removed);
}

static size_t rbsp_to_raw(const Rbsp *r, size_t pos) {
    size_t raw = pos;
    for (size_t i = 0; i < r->nremoved && r->removed[i] <= raw; i++) raw++;
    return raw;
}

static size_t raw_to_rbsp(const Rbsp *r, size_t raw) {
    size_t n = 0;
    while (n < r->nremoved && r->removed[n] < raw) n++;
    return raw - n;
}

// ---------------------------------------------------------------------------
// bit reader for the headers (u(n), ue(v), se(v))

typedef struct {
    const uint8_t *p;
    size_t len, pos;   // pos in bits
} Bits;

static uint32_t bits_u(Bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = b->pos >> 3;
        int bit = byte < b->len ? b->p[byte] >> (7 - (b->pos & 7)) & 1 : 0;
        v = v << 1 | (uint32_t)bit;
        b->pos++;
    }
    return v;
}

static uint32_t bits_ue(Bits *b) {
    int zeros = 0;
    while (!bits_u(b, 1)) {
        if (++zeros > 31) return 0xFFFFFFFF;
    }
    return (1u << zeros) - 1 + bits_u(b, zeros);
}

static int32_t bits_se(Bits *b) {
    uint32_t k = bits_ue(b);
    return k & 1 ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

// ---------------------------------------------------------------------------
// parameter sets (7.3.2)

typedef struct {
    int valid;
    int chroma_format_idc, width, height;
    int conf_left, conf_right, conf_top, conf_bottom;   // in luma samples
    int bit_depth_luma, bit_depth_chroma;
    int log2_max_poc_lsb;
    int log2_min_cb, log2_ctb, log2_min_tb, log2_max_tb;
    int max_transform_hierarchy_depth_intra;
    int scaling_list_enabled;
    uint8_t scaling[4][6][64 + 1];   // [sizeId][matrixId][coef in up-right diagonal order], [64]: DC
    int amp, sao, pcm;
    int num_short_term_ref_pic_sets;
    int long_term_ref_pics_present, num_long_term_ref_pics_sps;
    int temporal_mvp, strong_intra_smoothing;
} Sps;

typedef struct {
    int valid;
    int sps_id;
    int dependent_slice_segments, output_flag_present, num_extra_slice_header_bits;
    int sign_data_hiding, cabac_init_present, init_qp;
    int constrained_intra_pred, transform_skip;
    int cu_qp_delta_enabled, diff_cu_qp_delta_depth;
    int cb_qp_offset, cr_qp_offset, slice_chroma_qp_offsets_present;
    int transquant_bypass, tiles, entropy_coding_sync;
    int loop_filter_across_slices, deblocking_override_enabled, deblocking_disabled, beta_offset, tc_offset;
    int scaling_list_present;
    uint8_t scaling[4][6][64 + 1];
    int slice_header_extension_present;
} Pps;

static void skip_profile_tier_level(Bits *b, int max_sub_layers_minus1) {
    bits_u(b, 32);
    bits_u(b, 32);
    bits_u(b, 24);   // profile space ... general constraint flags (88 bits)
    bits_u(b, 8);    // general_level_idc
    int prof[8] = {0}, lev[8] = {0};
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        prof[i] = (int)bits_u(b, 1);
        lev[i] = (int)bits_u(b, 1);
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; i++) bits_u(b, 2);
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        if (prof[i]) {
            bits_u(b, 32);
            bits_u(b, 32);
            bits_u(b, 24);
        }
        if (lev[i]) bits_u(b, 8);
    }
}

// Table 7-5 / 7-6: the default scaling lists (in up-right diagonal order)
static const uint8_t default_intra8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21,
    19, 20, 21, 20, 19, 21, 24, 22, 22, 24, 24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29,
    31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115};
static const uint8_t default_inter8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20,
    20, 20, 20, 20, 20, 20, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28,
    28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91};

static void default_scaling(uint8_t s[4][6][65]) {
    for (int m = 0; m < 6; m++) {
        memset(s[0][m], 16, 65);
        for (int size = 1; size < 4; size++) {
            memcpy(s[size][m], m < 3 ? default_intra8 : default_inter8, 64);
            s[size][m][64] = 16;
        }
    }
}

// 7.3.4 scaling_list_data()
static int parse_scaling_list(Bits *b, uint8_t s[4][6][65]) {
    for (int size = 0; size < 4; size++) {
        for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
            int n = MIN(64, 1 << (4 + (size << 1)));
            if (!bits_u(b, 1)) {   // scaling_list_pred_mode_flag = 0: a copy or the default
                uint32_t delta = bits_ue(b);
                if (delta > (uint32_t)m / (size == 3 ? 3 : 1)) return -1;
                if (delta == 0) {
                    if (size == 0) memset(s[0][m], 16, 65);
                    else {
                        memcpy(s[size][m], m < 3 ? default_intra8 : default_inter8, 64);
                        s[size][m][64] = 16;
                    }
                } else {
                    int ref = m - (int)delta * (size == 3 ? 3 : 1);
                    memcpy(s[size][m], s[size][ref], 65);
                }
            } else {
                int next = 8;
                if (size > 1) {
                    int dc = bits_se(b) + 8;
                    if (dc < 1 || dc > 255) return -1;
                    next = dc;
                    s[size][m][64] = (uint8_t)dc;
                }
                for (int i = 0; i < n; i++) {
                    int d = bits_se(b);
                    next = (next + d + 256) % 256;
                    s[size][m][i] = (uint8_t)next;
                }
                if (size <= 1) s[size][m][64] = s[size][m][0];
            }
        }
    }
    // 32x32 chroma (4:4:4 only): copies of the 16x16 ones
    for (int m = 1; m < 6; m++)
        if (m != 3) memcpy(s[3][m], s[2][m], 65);
    return 0;
}

// 7.3.7 st_ref_pic_set(): only skipped (I pictures do not use it)
static int skip_st_ref_pic_set(Bits *b, int idx, int num_sets, int *num_delta) {
    int inter = 0;
    if (idx != 0) inter = (int)bits_u(b, 1);
    if (inter) {
        int delta_idx = 1;
        if (idx == num_sets) delta_idx = (int)bits_ue(b) + 1;
        bits_u(b, 1);   // delta_rps_sign
        bits_ue(b);     // abs_delta_rps_minus1
        int ref = idx - delta_idx;
        if (ref < 0) return -1;
        int n = 0;
        for (int j = 0; j <= num_delta[ref]; j++) {
            int used = (int)bits_u(b, 1);
            int use_delta = 1;
            if (!used) use_delta = (int)bits_u(b, 1);
            if (used || use_delta) n++;
        }
        num_delta[idx] = n;
    } else {
        uint32_t neg = bits_ue(b), pos = bits_ue(b);
        if (neg > 16 || pos > 16) return -1;
        for (uint32_t i = 0; i < neg + pos; i++) {
            bits_ue(b);
            bits_u(b, 1);
        }
        num_delta[idx] = (int)(neg + pos);
    }
    return 0;
}

static int parse_sps(const uint8_t *rbsp, size_t len, Sps *sps_table) {
    Bits b = {rbsp, len, 16};   // (after the NAL unit header)
    bits_u(&b, 4);              // sps_video_parameter_set_id
    int max_sub_layers_minus1 = (int)bits_u(&b, 3);
    bits_u(&b, 1);
    skip_profile_tier_level(&b, max_sub_layers_minus1);
    uint32_t id = bits_ue(&b);
    if (id > 15) return -1;
    Sps *s = &sps_table[id];
    memset(s, 0, sizeof *s);
    s->chroma_format_idc = (int)bits_ue(&b);
    if (s->chroma_format_idc == 3) bits_u(&b, 1);
    s->width = (int)bits_ue(&b);
    s->height = (int)bits_ue(&b);
    if (bits_u(&b, 1)) {   // conformance window (in chroma units for 4:2:0)
        int sub = s->chroma_format_idc == 1 || s->chroma_format_idc == 2 ? 2 : 1;
        int subh = s->chroma_format_idc == 1 ? 2 : 1;
        s->conf_left = (int)bits_ue(&b) * sub;
        s->conf_right = (int)bits_ue(&b) * sub;
        s->conf_top = (int)bits_ue(&b) * subh;
        s->conf_bottom = (int)bits_ue(&b) * subh;
    }
    s->bit_depth_luma = (int)bits_ue(&b) + 8;
    s->bit_depth_chroma = (int)bits_ue(&b) + 8;
    s->log2_max_poc_lsb = (int)bits_ue(&b) + 4;
    int ordering = (int)bits_u(&b, 1);
    for (int i = ordering ? 0 : max_sub_layers_minus1; i <= max_sub_layers_minus1; i++) {
        bits_ue(&b);
        bits_ue(&b);
        bits_ue(&b);
    }
    s->log2_min_cb = (int)bits_ue(&b) + 3;
    s->log2_ctb = s->log2_min_cb + (int)bits_ue(&b);
    s->log2_min_tb = (int)bits_ue(&b) + 2;
    s->log2_max_tb = s->log2_min_tb + (int)bits_ue(&b);
    bits_ue(&b);   // max_transform_hierarchy_depth_inter
    s->max_transform_hierarchy_depth_intra = (int)bits_ue(&b);
    s->scaling_list_enabled = (int)bits_u(&b, 1);
    default_scaling(s->scaling);
    if (s->scaling_list_enabled && bits_u(&b, 1) && parse_scaling_list(&b, s->scaling)) return -1;
    s->amp = (int)bits_u(&b, 1);
    s->sao = (int)bits_u(&b, 1);
    s->pcm = (int)bits_u(&b, 1);
    if (s->pcm) {
        bits_u(&b, 8);
        bits_ue(&b);
        bits_ue(&b);
        bits_u(&b, 1);
    }
    s->num_short_term_ref_pic_sets = (int)bits_ue(&b);
    if (s->num_short_term_ref_pic_sets > 64) return -1;
    int num_delta[65] = {0};
    for (int i = 0; i < s->num_short_term_ref_pic_sets; i++)
        if (skip_st_ref_pic_set(&b, i, s->num_short_term_ref_pic_sets, num_delta)) return -1;
    s->long_term_ref_pics_present = (int)bits_u(&b, 1);
    if (s->long_term_ref_pics_present) {
        s->num_long_term_ref_pics_sps = (int)bits_ue(&b);
        for (int i = 0; i < s->num_long_term_ref_pics_sps; i++) {
            bits_u(&b, s->log2_max_poc_lsb);
            bits_u(&b, 1);
        }
    }
    s->temporal_mvp = (int)bits_u(&b, 1);
    s->strong_intra_smoothing = (int)bits_u(&b, 1);
    // (VUI and extensions: not needed)
    if (s->width < 8 || s->height < 8 || s->width > 16384 || s->height > 16384 || s->log2_ctb < 4 || s->log2_ctb > 6 ||
        s->log2_max_tb > 5 || s->log2_max_tb > s->log2_ctb || s->log2_min_tb >= s->log2_min_cb ||
        (s->width & ((1 << s->log2_min_cb) - 1)) || (s->height & ((1 << s->log2_min_cb) - 1)))
        return -1;
    s->valid = 1;
    return 0;
}

static int parse_pps(const uint8_t *rbsp, size_t len, const Sps *sps_table, Pps *pps_table) {
    Bits b = {rbsp, len, 16};
    uint32_t id = bits_ue(&b);
    if (id > 63) return -1;
    Pps *p = &pps_table[id];
    memset(p, 0, sizeof *p);
    p->sps_id = (int)bits_ue(&b);
    if (p->sps_id > 15 || !sps_table[p->sps_id].valid) return -1;
    p->dependent_slice_segments = (int)bits_u(&b, 1);
    p->output_flag_present = (int)bits_u(&b, 1);
    p->num_extra_slice_header_bits = (int)bits_u(&b, 3);
    p->sign_data_hiding = (int)bits_u(&b, 1);
    p->cabac_init_present = (int)bits_u(&b, 1);
    bits_ue(&b);
    bits_ue(&b);
    p->init_qp = 26 + bits_se(&b);
    p->constrained_intra_pred = (int)bits_u(&b, 1);
    p->transform_skip = (int)bits_u(&b, 1);
    p->cu_qp_delta_enabled = (int)bits_u(&b, 1);
    if (p->cu_qp_delta_enabled) p->diff_cu_qp_delta_depth = (int)bits_ue(&b);
    p->cb_qp_offset = bits_se(&b);
    p->cr_qp_offset = bits_se(&b);
    p->slice_chroma_qp_offsets_present = (int)bits_u(&b, 1);
    bits_u(&b, 1);   // weighted_pred_flag
    bits_u(&b, 1);   // weighted_bipred_flag
    p->transquant_bypass = (int)bits_u(&b, 1);
    p->tiles = (int)bits_u(&b, 1);
    p->entropy_coding_sync = (int)bits_u(&b, 1);
    if (p->tiles) return -1;   // (tiles inside a picture: not handled yet)
    p->loop_filter_across_slices = (int)bits_u(&b, 1);
    if (bits_u(&b, 1)) {   // deblocking_filter_control_present_flag
        p->deblocking_override_enabled = (int)bits_u(&b, 1);
        p->deblocking_disabled = (int)bits_u(&b, 1);
        if (!p->deblocking_disabled) {
            p->beta_offset = bits_se(&b) * 2;
            p->tc_offset = bits_se(&b) * 2;
        }
    }
    const Sps *s = &sps_table[p->sps_id];
    memcpy(p->scaling, s->scaling, sizeof p->scaling);
    p->scaling_list_present = (int)bits_u(&b, 1);
    if (p->scaling_list_present) {
        default_scaling(p->scaling);
        if (parse_scaling_list(&b, p->scaling)) return -1;
    }
    bits_u(&b, 1);   // lists_modification_present_flag
    bits_ue(&b);     // log2_parallel_merge_level_minus2
    p->slice_header_extension_present = (int)bits_u(&b, 1);
    if (p->init_qp < 0 || p->init_qp > 51 || p->cb_qp_offset < -12 || p->cb_qp_offset > 12 || p->cr_qp_offset < -12 ||
        p->cr_qp_offset > 12)
        return -1;
    p->valid = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// CABAC (9.3)

enum {   // context variables of the syntax elements an I slice uses
    CTX_SAO_MERGE = 0,
    CTX_SAO_TYPE = 1,
    CTX_SPLIT_CU = 2,                 // 3
    CTX_TQ_BYPASS = 5,
    CTX_PART_MODE = 6,
    CTX_PREV_INTRA = 7,
    CTX_CHROMA_MODE = 8,
    CTX_SPLIT_TRANSFORM = 9,          // 3
    CTX_CBF_LUMA = 12,                // 2
    CTX_CBF_CHROMA = 14,              // 5
    CTX_CU_QP_DELTA = 19,             // 2
    CTX_TS_FLAG = 21,                 // 2
    CTX_LAST_X = 23,                  // 18
    CTX_LAST_Y = 41,                  // 18
    CTX_CSBF = 59,                    // 4
    CTX_SIG = 63,                     // 42 (+2)
    CTX_GT1 = 107,                    // 24
    CTX_GT2 = 131,                    // 6
    NUM_CTX = 137
};

// initValue of each context for initType 0 (I slices), Tables 9-5 ... 9-37
static const uint8_t init_values[NUM_CTX] = {
    153,                                                                              // sao_merge
    200,                                                                              // sao_type_idx
    139, 141, 157,                                                                    // split_cu_flag
    154,                                                                              // cu_transquant_bypass_flag
    184,                                                                              // part_mode
    184,                                                                              // prev_intra_luma_pred_flag
    63,                                                                               // intra_chroma_pred_mode
    153, 138, 138,                                                                    // split_transform_flag
    111, 141,                                                                         // cbf_luma
    94, 138, 182, 154, 154,                                                           // cbf_cb / cbf_cr
    154, 154,                                                                         // cu_qp_delta_abs
    139, 139,                                                                         // transform_skip_flag
    110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111, 79, 108, 123, 63,   // last_x
    110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111, 79, 108, 123, 63,   // last_y
    91, 171, 134, 141,                                                                // coded_sub_block_flag
    111, 111, 125, 110, 110, 94, 124, 108, 124, 107, 125, 141, 179, 153, 125, 107, 125, 141, 179, 153, 125,
    107, 125, 141, 179, 153, 125, 140, 139, 182, 182, 152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111,
    141, 111,                                                                         // sig_coeff_flag
    140, 92, 137, 138, 140, 152, 138, 139, 153, 74, 149, 92, 139, 107, 122, 152, 140, 179, 166, 182, 140, 227,
    122, 197,                                                                         // greater1
    138, 153, 136, 167, 152, 152,                                                     // greater2
};

static const uint8_t range_lps[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216}, {123, 150, 178, 205}, {116, 142, 169, 195},
    {111, 135, 160, 185}, {105, 128, 152, 175}, {100, 122, 144, 166}, {95, 116, 137, 158},  {90, 110, 130, 150},
    {85, 104, 123, 142},  {81, 99, 117, 135},   {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},
    {66, 80, 95, 110},    {62, 76, 90, 104},    {59, 72, 86, 99},     {56, 69, 81, 94},     {53, 65, 77, 89},
    {51, 62, 73, 85},     {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},     {41, 50, 59, 69},
    {39, 48, 56, 65},     {37, 45, 54, 62},     {35, 43, 51, 59},     {33, 41, 48, 56},     {32, 39, 46, 53},
    {30, 37, 43, 50},     {29, 35, 41, 48},     {27, 33, 39, 45},     {26, 31, 37, 43},     {24, 30, 35, 41},
    {23, 28, 33, 39},     {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},     {19, 23, 27, 31},
    {18, 22, 26, 30},     {17, 21, 25, 28},     {16, 20, 23, 27},     {15, 19, 22, 25},     {14, 18, 21, 24},
    {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},     {12, 14, 17, 20},     {11, 14, 16, 19},
    {11, 13, 15, 18},     {10, 12, 15, 17},     {10, 12, 14, 16},     {9, 11, 13, 15},      {9, 11, 12, 14},
    {8, 10, 12, 14},      {8, 9, 11, 13},       {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},         {2, 2, 2, 2}};

static const uint8_t trans_idx_lps[64] = {0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
                                          13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
                                          24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
                                          33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};

// The arithmetic decoder keeps ivlOffset scaled: value = ivlOffset << 7 plus up to 8 bits read
// ahead (bits_needed: -8 .. -1 how many of them are still free), so bytes are read one at a
// time instead of bit by bit; range and value are compared with the range scaled the same way.
typedef struct {
    const uint8_t *p, *end;
    uint32_t range, value;
    int bits_needed;
    uint8_t state[NUM_CTX];   // pStateIdx << 1 | valMps
} Cabac;

static inline uint32_t cabac_byte(Cabac *c) { return c->p < c->end ? *c->p++ : 0; }

static void cabac_start(Cabac *c, const uint8_t *p, const uint8_t *end) {   // 9.3.2.5
    c->p = p;
    c->end = end;
    c->range = 510;
    c->bits_needed = -8;
    c->value = cabac_byte(c) << 8;
    c->value |= cabac_byte(c);
}

static void cabac_init_contexts(Cabac *c, int qp) {   // 9.3.2.2
    qp = CLIP3(0, 51, qp);
    for (int i = 0; i < NUM_CTX; i++) {
        int slope = init_values[i] >> 4, off = init_values[i] & 15;
        int m = slope * 5 - 45, n = (off << 3) - 16;
        int pre = CLIP3(1, 126, ((m * qp) >> 4) + n);
        c->state[i] = (uint8_t)(pre <= 63 ? (63 - pre) << 1 : (pre - 64) << 1 | 1);
    }
}

static inline int cabac_decision(Cabac *c, int ctx) {   // 9.3.4.3.2
    uint8_t s = c->state[ctx];
    int p = s >> 1, mps = s & 1;
    uint32_t lps = range_lps[p][(c->range >> 6) & 3];
    c->range -= lps;
    uint32_t scaled = c->range << 7;
    int bin;
    if (c->value < scaled) {   // MPS
        bin = mps;
        if (p < 62) p++;
        if (scaled < (256u << 7)) {   // one bit of renormalization
            c->range = scaled >> 6;
            c->value <<= 1;
            if (++c->bits_needed == 0) {
                c->bits_needed = -8;
                c->value |= cabac_byte(c);
            }
        }
    } else {   // LPS
        bin = !mps;
        c->value -= scaled;
        int shift = __builtin_clz(lps) - 23;   // lps back to >= 256
        c->value <<= shift;
        c->range = lps << shift;
        if (p == 0) mps = !mps;
        p = trans_idx_lps[p];
        c->bits_needed += shift;
        if (c->bits_needed >= 0) {
            c->value |= cabac_byte(c) << c->bits_needed;
            c->bits_needed -= 8;
        }
    }
    c->state[ctx] = (uint8_t)(p << 1 | mps);
    return bin;
}

static inline int cabac_bypass(Cabac *c) {   // 9.3.4.3.4
    c->value <<= 1;
    if (++c->bits_needed >= 0) {
        c->bits_needed = -8;
        c->value |= cabac_byte(c);
    }
    uint32_t scaled = c->range << 7;
    if (c->value >= scaled) {
        c->value -= scaled;
        return 1;
    }
    return 0;
}

static uint32_t cabac_bypass_bits(Cabac *c, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 1 | (uint32_t)cabac_bypass(c);
    return v;
}

static int cabac_terminate(Cabac *c) {   // 9.3.4.3.5
    c->range -= 2;
    uint32_t scaled = c->range << 7;
    if (c->value >= scaled) return 1;
    if (scaled < (256u << 7)) {
        c->range = scaled >> 6;
        c->value <<= 1;
        if (++c->bits_needed == 0) {
            c->bits_needed = -8;
            c->value |= cabac_byte(c);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// scan orders (6.5.3 - 6.5.5): [log2 size 0..3][scanIdx] -> (x, y) pairs

static uint8_t scan_order[4][3][64][2];
static uint8_t diag_index[2][8][8];   // [4x4 / 8x8][y][x] -> position in the up-right diagonal scan

static inline int raster_to_diag(int x, int y, int size) { return diag_index[size == 8][y][x]; }

static void init_scans(void) {
    for (int log2 = 0; log2 < 4; log2++) {
        int n = 1 << log2, i = 0, x = 0, y = 0;
        while (i < n * n) {   // up-right diagonal
            while (y >= 0) {
                if (x < n && y < n) {
                    scan_order[log2][0][i][0] = (uint8_t)x;
                    scan_order[log2][0][i][1] = (uint8_t)y;
                    i++;
                }
                y--;
                x++;
            }
            y = x;
            x = 0;
        }
        if (log2 == 2 || log2 == 3)
            for (int k = 0; k < n * n; k++)
                diag_index[log2 == 3][scan_order[log2][0][k][1]][scan_order[log2][0][k][0]] = (uint8_t)k;
        i = 0;
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++) {   // horizontal
                scan_order[log2][1][i][0] = (uint8_t)x;
                scan_order[log2][1][i][1] = (uint8_t)y;
                i++;
            }
        i = 0;
        for (x = 0; x < n; x++)
            for (y = 0; y < n; y++) {   // vertical
                scan_order[log2][2][i][0] = (uint8_t)x;
                scan_order[log2][2][i][1] = (uint8_t)y;
                i++;
            }
    }
}

// ---------------------------------------------------------------------------
// transforms (8.6.4)

static int8_t dct32[32][32];   // [frequency][sample]

static void init_dct(void) {
    // integer approximations of 64 * sqrt(2) * cos(j * pi / 64), j = 0..32 (j = 0: the DC row's 64)
    static const int t[33] = {64, 90, 90, 90, 89, 88, 87, 85, 83, 82, 80, 78, 75, 73, 70, 67, 64,
                              61, 57, 54, 50, 46, 43, 38, 36, 31, 25, 22, 18, 13, 9,  4,  0};
    for (int k = 0; k < 32; k++)
        for (int n = 0; n < 32; n++) {
            if (k == 0) {
                dct32[k][n] = 64;
                continue;
            }
            int a = k * (2 * n + 1) % 128;   // angle in units of pi / 64
            if (a > 64) a = 128 - a;
            dct32[k][n] = (int8_t)(a <= 32 ? t[a] : -t[64 - a]);
        }
}

static const int8_t dst4[4][4] = {{29, 55, 74, 84}, {74, 74, 0, -74}, {84, -29, -74, 55}, {55, -84, 74, -29}};

// inverse transform of an n x n block (coefficients c[y * n + x], zero outside columns 0..maxx and
// rows 0..maxy) into residuals; the loops over a matrix row vectorize
static void inverse_transform(const int32_t *c, int log2, int dst, int maxx, int maxy, int16_t *res) {
    int n = 1 << log2, step = 32 >> log2;
    if (!dst && maxx == 0 && maxy == 0) {   // DC only: a constant
        int g = (64 * c[0] + 64) >> 7;
        g = CLIP3(-32768, 32767, g);
        int16_t v = (int16_t)((64 * g + 2048) >> 12);
        for (int i = 0; i < n * n; i++) res[i] = v;
        return;
    }
    int32_t tmp[32 * 32];   // [y * n + x]
    int32_t acc[32];
    if (dst) {   // 4x4 DST: directly
        for (int x = 0; x <= maxx; x++) {
            for (int y = 0; y < 4; y++) acc[y] = 0;
            for (int j = 0; j <= maxy; j++)
                for (int y = 0; y < 4; y++) acc[y] += dst4[j][y] * c[j * 4 + x];
            for (int y = 0; y < 4; y++) tmp[y * 4 + x] = CLIP3(-32768, 32767, (acc[y] + 64) >> 7);
        }
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) acc[x] = 0;
            for (int j = 0; j <= maxx; j++)
                for (int x = 0; x < 4; x++) acc[x] += dst4[j][x] * tmp[y * 4 + j];
            for (int x = 0; x < 4; x++) res[y * 4 + x] = (int16_t)((acc[x] + 2048) >> 12);
        }
        return;
    }
    // DCT: the even basis functions are symmetric, the odd ones antisymmetric, so only the first
    // half is computed: out[i] = E[i] + O[i], out[n - 1 - i] = E[i] - O[i]
    int h = n / 2;
    int32_t ev[16], od[16];
    // columns: e[x][y] = sum over j of M[j][y] * d[x][j], then (e + 64) >> 7, clipped to 16 bits
    for (int x = 0; x <= maxx; x++) {
        for (int y = 0; y < h; y++) ev[y] = od[y] = 0;
        for (int j = 0; j <= maxy; j++) {
            int32_t v = c[j * n + x];
            if (!v) continue;
            const int8_t *m = dct32[j * step];
            int32_t *a = j & 1 ? od : ev;
            for (int y = 0; y < h; y++) a[y] += m[y] * v;
        }
        for (int y = 0; y < h; y++) {
            tmp[y * n + x] = CLIP3(-32768, 32767, (ev[y] + od[y] + 64) >> 7);
            tmp[(n - 1 - y) * n + x] = CLIP3(-32768, 32767, (ev[y] - od[y] + 64) >> 7);
        }
    }
    // rows (only columns 0..maxx are not zero), then (r + 2048) >> 12 (bdShift = 20 - BitDepth)
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < h; x++) ev[x] = od[x] = 0;
        for (int j = 0; j <= maxx; j++) {
            int32_t v = tmp[y * n + j];
            if (!v) continue;
            const int8_t *m = dct32[j * step];
            int32_t *a = j & 1 ? od : ev;
            for (int x = 0; x < h; x++) a[x] += m[x] * v;
        }
        for (int x = 0; x < h; x++) {
            res[y * n + x] = (int16_t)((ev[x] + od[x] + 2048) >> 12);
            res[y * n + n - 1 - x] = (int16_t)((ev[x] - od[x] + 2048) >> 12);
        }
    }
    (void)acc;
}

// ---------------------------------------------------------------------------
// decoder state

typedef struct {
    const Sps *sps;
    const Pps *pps;
    int w, h;                  // picture size in luma samples
    int ctb_w, ctb_h;          // in CTBs
    int log2_ctb, ctb;
    uint8_t *plane[3];
    int stride[3];
    // per 4x4 luma block
    int w4, h4;
    uint8_t *decoded;          // reconstructed (for the availability of intra reference samples)
    uint8_t *intra_mode;       // IntraPredModeY
    int8_t *qp;                // QpY
    uint8_t *ct_depth;         // coding tree depth
    int *slice_addr;           // per CTB: the address of its slice's first CTB (-1: not decoded)
    // slice
    int slice_addr_rs, slice_qp, cb_qp_offset, cr_qp_offset;
    int sao_luma, sao_chroma;
    // coding unit / quantization group
    int cu_transquant_bypass;
    int is_cu_qp_delta_coded, cu_qp_delta;
    int qp_pred, qp_prev_cu, qp_y;
    int intra_chroma_mode;     // of the current CU (final mode, 0..34)
    Cabac cabac;
    int16_t res[32 * 32];
    int32_t coeff[32 * 32];
    uint16_t nz[32 * 32];        // positions (y * n + x) of the coefficients of the current block
    uint8_t sf[4][3][32 * 32];   // ScalingFactor (7.4.5): [log2 - 2][cIdx][y * n + x], intra matrices
    int sf_ready;
} Dec;

// ScalingFactor of the picture's PPS (flat 16 without scaling lists)
static void scaling_factors(Dec *d) {
    for (int log2 = 2; log2 <= 5; log2++) {
        int n = 1 << log2;
        for (int c = 0; c < 3; c++) {
            const uint8_t *sl = d->pps->scaling[log2 - 2][c];
            uint8_t *f = d->sf[log2 - 2][c];
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    int m = 16;
                    if (d->sps->scaling_list_enabled) {   // the 8x8 (or 4x4) list upsampled, [0][0]: DC
                        int r = log2 == 2 ? 0 : log2 - 3;
                        m = sl[raster_to_diag(x >> r, y >> r, log2 == 2 ? 4 : 8)];
                        if (log2 >= 4 && x == 0 && y == 0) m = sl[64];
                    }
                    f[y * n + x] = (uint8_t)m;
                }
        }
    }
}

static inline int avail4(const Dec *d, int x, int y, int cur_ctb_slice) {
    // luma sample (x, y): inside the picture, reconstructed and in the same slice
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return 0;
    if (!d->decoded[(y >> 2) * d->w4 + (x >> 2)]) return 0;
    return d->slice_addr[(y >> d->log2_ctb) * d->ctb_w + (x >> d->log2_ctb)] == cur_ctb_slice;
}

// ---------------------------------------------------------------------------
// intra prediction (8.4.4.2)

static const int8_t intra_angle[35] = {0,   0,   32,  26,  21,  17,  13,  9,  5,  2,  0,  -2,
                                       -5,  -9,  -13, -17, -21, -26, -32, -26, -21, -17, -13, -9,
                                       -5,  -2,  0,   2,   5,   9,   13,  17,  21,  26,  32};
static const int16_t inv_angle[35] = {0,     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    -4096,
                                      -1638, -910, -630, -482, -390, -315, -256, -315, -390, -482, -630, -910,
                                      -1638, -4096};

// predicts the n x n block at (x0, y0) of component c (chroma: chroma sample units)
static void intra_predict(Dec *d, int c, int x0, int y0, int log2, int mode) {
    int n = 1 << log2;
    int sub = c ? 1 : 0;   // chroma -> luma coordinates: << 1 (4:2:0)
    int stride = d->stride[c];
    uint8_t *dst = d->plane[c] + (size_t)y0 * stride + x0;
    int cur_slice = d->slice_addr_rs;
    // reference samples: left[0..2n] = p[-1][-1 .. 2n-1], top[0..2n] = p[-1 .. 2n-1][-1]
    int left[65], top[65];
    int avl[65], avt[65];
    int any = 0;
    for (int i = 0; i <= 2 * n; i++) {
        int yy = y0 - 1 + i;   // left column, from the corner down
        int ok = avail4(d, (x0 - 1) << sub, yy << sub, cur_slice);
        avl[i] = ok;
        if (ok) left[i] = d->plane[c][(size_t)yy * stride + x0 - 1];
        any |= ok;
        int xx = x0 - 1 + i;   // top row, from the corner right
        ok = avail4(d, xx << sub, (y0 - 1) << sub, cur_slice);
        avt[i] = ok;
        if (ok) top[i] = d->plane[c][(size_t)(y0 - 1) * stride + xx];
        any |= ok;
    }
    // substitution (8.4.4.2.2): order p[-1][2n-1] .. p[-1][-1], p[0][-1] .. p[2n-1][-1]
    if (!any) {
        for (int i = 0; i <= 2 * n; i++) left[i] = top[i] = 128;
    } else {
        // the sequence: left[2n], left[2n-1], ..., left[0] (= corner = top[0]), top[1] ... top[2n]
        if (!avl[2 * n]) {
            int v = -1;
            for (int i = 2 * n - 1; i >= 0 && v < 0; i--)
                if (avl[i]) v = left[i];
            for (int i = 1; i <= 2 * n && v < 0; i++)
                if (avt[i]) v = top[i];
            left[2 * n] = v;
            avl[2 * n] = 1;
        }
        for (int i = 2 * n - 1; i >= 0; i--)
            if (!avl[i]) left[i] = left[i + 1];
        top[0] = left[0];
        for (int i = 1; i <= 2 * n; i++)
            if (!avt[i]) top[i] = top[i - 1];
    }
    // filtering of the reference samples (8.4.4.2.3): luma only (4:2:0)
    if (c == 0 && mode != 1 && n != 4) {
        int dist = MIN(abs(mode - 26), abs(mode - 10));
        int thres = n == 8 ? 7 : n == 16 ? 1 : 0;
        if (dist > thres) {
            int fl[65], ft[65];
            int corner = left[0];
            if (d->sps->strong_intra_smoothing && n == 32 &&
                abs(corner + top[2 * n] - 2 * top[n]) < (1 << (8 - 5)) &&
                abs(corner + left[2 * n] - 2 * left[n]) < (1 << (8 - 5))) {
                fl[0] = ft[0] = corner;
                for (int i = 1; i < 64; i++) {
                    fl[i] = ((64 - i) * corner + i * left[64] + 32) >> 6;
                    ft[i] = ((64 - i) * corner + i * top[64] + 32) >> 6;
                }
                fl[64] = left[64];
                ft[64] = top[64];
            } else {
                fl[0] = ft[0] = (left[1] + 2 * corner + top[1] + 2) >> 2;
                for (int i = 1; i < 2 * n; i++) {
                    fl[i] = (left[i + 1] + 2 * left[i] + left[i - 1] + 2) >> 2;
                    ft[i] = (top[i + 1] + 2 * top[i] + top[i - 1] + 2) >> 2;
                }
                fl[2 * n] = left[2 * n];
                ft[2 * n] = top[2 * n];
            }
            memcpy(left, fl, sizeof(int) * (2 * n + 1));
            memcpy(top, ft, sizeof(int) * (2 * n + 1));
        }
    }
    // p[-1][y] = left[y + 1], p[x][-1] = top[x + 1], p[-1][-1] = left[0]
    if (mode == 0) {   // planar
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[y * stride + x] = (uint8_t)(((n - 1 - x) * left[y + 1] + (x + 1) * top[n + 1] +
                                                 (n - 1 - y) * top[x + 1] + (y + 1) * left[n + 1] + n) >>
                                                (log2 + 1));
    } else if (mode == 1) {   // DC
        int sum = n;
        for (int i = 1; i <= n; i++) sum += left[i] + top[i];
        int dc = sum >> (log2 + 1);
        for (int y = 0; y < n; y++) memset(dst + y * stride, dc, (size_t)n);
        if (c == 0 && n < 32) {
            dst[0] = (uint8_t)((left[1] + 2 * dc + top[1] + 2) >> 2);
            for (int x = 1; x < n; x++) dst[x] = (uint8_t)((top[x + 1] + 3 * dc + 2) >> 2);
            for (int y = 1; y < n; y++) dst[y * stride] = (uint8_t)((left[y + 1] + 3 * dc + 2) >> 2);
        }
    } else {   // angular
        int angle = intra_angle[mode];
        int refbuf[3 * 64 + 1];
        int *ref = refbuf + 64;   // ref[-n .. 2n]
        int vertical = mode >= 18;
        const int *main = vertical ? top : left, *side = vertical ? left : top;
        for (int x = 0; x <= n; x++) ref[x] = main[x];   // ref[x] = p[-1 + x][-1] (vertical)
        if (angle < 0) {
            int last = (n * angle) >> 5;
            if (last < -1)
                for (int x = last; x <= -1; x++) ref[x] = side[((x * inv_angle[mode] + 128) >> 8)];
        } else {
            for (int x = n + 1; x <= 2 * n; x++) ref[x] = main[x];
        }
        for (int y = 0; y < n; y++) {
            int pos = (y + 1) * angle, idx = pos >> 5, fact = pos & 31;
            for (int x = 0; x < n; x++) {
                int v = fact ? ((32 - fact) * ref[x + idx + 1] + fact * ref[x + idx + 2] + 16) >> 5 : ref[x + idx + 1];
                if (vertical) dst[y * stride + x] = (uint8_t)v;
                else dst[x * stride + y] = (uint8_t)v;
            }
        }
        if (c == 0 && n < 32) {
            if (mode == 26)
                for (int y = 0; y < n; y++) dst[y * stride] = clip_pixel(top[1] + ((left[y + 1] - left[0]) >> 1));
            if (mode == 10)
                for (int x = 0; x < n; x++) dst[x] = clip_pixel(left[1] + ((top[x + 1] - top[0]) >> 1));
        }
    }
}

// ---------------------------------------------------------------------------
// residual coding (7.3.8.11, 9.3.4.2.x) and dequantization (8.6.2, 8.6.3)

static const uint8_t ctx_idx_map4[16] = {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8};
static const int level_scale[6] = {40, 45, 51, 57, 64, 72};

static int chroma_qp_table(int qpi) {   // Table 8-10 (ChromaArrayType 1)
    static const int8_t t[14] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37};
    return qpi < 30 ? qpi : qpi > 43 ? qpi - 6 : t[qpi - 30];
}

// decodes the coefficients of a transform block, dequantizes, transforms and adds them
// to the prediction at (x0, y0) of component c (in that component's samples)
static int residual(Dec *d, int x0, int y0, int log2, int c, int pred_mode) {
    Cabac *cb = &d->cabac;
    int n = 1 << log2;
    int32_t *coef = d->coeff;   // (all zero between blocks: cleared after use)
    if (d->pps->transform_skip) return -1;   // (transform_skip_flag: not handled yet)

    // scanIdx (7.4.9.11)
    int scan = 0;
    if (log2 == 2 || (log2 == 3 && c == 0)) {
        if (pred_mode >= 6 && pred_mode <= 14) scan = 2;
        else if (pred_mode >= 22 && pred_mode <= 30) scan = 1;
    }
    // last significant coefficient
    int ctx_off, ctx_shift;
    if (c == 0) {
        ctx_off = 3 * (log2 - 2) + ((log2 - 1) >> 2);
        ctx_shift = (log2 + 1) >> 2;
    } else {
        ctx_off = 15;
        ctx_shift = log2 - 2;
    }
    int max = (log2 << 1) - 1, px = 0, py = 0;
    while (px < max && cabac_decision(cb, CTX_LAST_X + ctx_off + (px >> ctx_shift))) px++;
    while (py < max && cabac_decision(cb, CTX_LAST_Y + ctx_off + (py >> ctx_shift))) py++;
    int last_x = px, last_y = py;
    if (px > 3) {
        int k = (px >> 1) - 1;
        last_x = (1 << k) * (2 + (px & 1)) + (int)cabac_bypass_bits(cb, k);
    }
    if (py > 3) {
        int k = (py >> 1) - 1;
        last_y = (1 << k) * (2 + (py & 1)) + (int)cabac_bypass_bits(cb, k);
    }
    if (scan == 2) {
        int t = last_x;
        last_x = last_y;
        last_y = t;
    }
    if (last_x >= n || last_y >= n) return -1;

    int log2sb = log2 - 2, nsb = 1 << log2sb;
    const uint8_t(*sb_scan)[2] = scan_order[log2sb][scan];
    const uint8_t(*pos_scan)[2] = scan_order[2][scan];
    // the sub-block and position of the last coefficient
    int last_sb = (1 << (2 * log2sb)) - 1, last_pos = 16;
    for (;;) {
        if (last_pos == 0) {
            last_pos = 16;
            last_sb--;
            if (last_sb < 0) return -1;
        }
        last_pos--;
        int xc = (sb_scan[last_sb][0] << 2) + pos_scan[last_pos][0];
        int yc = (sb_scan[last_sb][1] << 2) + pos_scan[last_pos][1];
        if (xc == last_x && yc == last_y) break;
    }

    uint8_t csbf[8][8];
    memset(csbf, 0, sizeof csbf);
    int nnz = 0, maxx = 0, maxy = 0;
    int greater1_state = 1;   // (the "c1" state carried between sub-blocks)
    int sign_hiding = d->pps->sign_data_hiding && !d->cu_transquant_bypass;
    for (int i = last_sb; i >= 0; i--) {
        int xs = sb_scan[i][0], ys = sb_scan[i][1];
        int infer_dc = 0;
        if (i < last_sb && i > 0) {
            int ctx = 0;
            if (xs < nsb - 1) ctx += csbf[xs + 1][ys];
            if (ys < nsb - 1) ctx += csbf[xs][ys + 1];
            csbf[xs][ys] = (uint8_t)cabac_decision(cb, CTX_CSBF + MIN(ctx, 1) + (c ? 2 : 0));
            infer_dc = 1;
        } else {
            csbf[xs][ys] = 1;
        }
        // significant coefficients of the sub-block, in scan order n = 15 .. 0
        int sig[16], nsig = 0;   // scan positions, from the highest
        int start = i == last_sb ? last_pos - 1 : 15;
        if (i == last_sb) sig[nsig++] = last_pos;
        if (csbf[xs][ys]) {
            int prev = 0;
            if (xs < nsb - 1) prev += csbf[xs + 1][ys];
            if (ys < nsb - 1) prev += csbf[xs][ys + 1] << 1;
            for (int p = start; p >= 0; p--) {
                int xp = pos_scan[p][0], yp = pos_scan[p][1];
                int xc = (xs << 2) + xp, yc = (ys << 2) + yp;
                if (p == 0 && infer_dc) {   // inferred significant: nothing coded after it
                    sig[nsig++] = 0;
                    break;
                }
                int sctx;
                if (log2 == 2) sctx = ctx_idx_map4[(yc << 2) + xc];
                else if (xc + yc == 0) sctx = 0;
                else {
                    if (prev == 0) sctx = xp + yp == 0 ? 2 : xp + yp < 3 ? 1 : 0;
                    else if (prev == 1) sctx = yp == 0 ? 2 : yp == 1 ? 1 : 0;
                    else if (prev == 2) sctx = xp == 0 ? 2 : xp == 1 ? 1 : 0;
                    else sctx = 2;
                    if (c == 0) {
                        if (xs + ys > 0) sctx += 3;
                        sctx += log2 == 3 ? (scan == 0 ? 9 : 15) : 21;
                    } else {
                        sctx += log2 == 3 ? 9 : 12;
                    }
                }
                if (cabac_decision(cb, CTX_SIG + (c ? 27 + sctx : sctx))) {
                    sig[nsig++] = p;
                    infer_dc = 0;
                }
            }
        }
        if (!nsig) continue;
        // levels: greater1 (first 8), greater2 (first with greater1), signs, remaining
        int ctx_set = (i == 0 || c > 0) ? 0 : 2;
        if (greater1_state == 0) ctx_set++;
        greater1_state = 1;
        int g1[16] = {0}, g2_pos = -1;
        int ng1 = MIN(nsig, 8);
        for (int k = 0; k < ng1; k++) {
            int ctx = ctx_set * 4 + greater1_state + (c ? 16 : 0);
            g1[k] = cabac_decision(cb, CTX_GT1 + ctx);
            if (g1[k]) {
                greater1_state = 0;
                if (g2_pos < 0) g2_pos = k;
            } else if (greater1_state > 0 && greater1_state < 3) {
                greater1_state++;
            }
        }
        int g2 = 0;
        if (g2_pos >= 0) g2 = cabac_decision(cb, CTX_GT2 + ctx_set + (c ? 4 : 0));
        int hidden = sign_hiding && sig[0] - sig[nsig - 1] > 3;
        uint32_t signs = cabac_bypass_bits(cb, hidden ? nsig - 1 : nsig) << (hidden ? 1 : 0);   // sig[0] first
        int rice = 0, sum = 0;
        for (int k = 0; k < nsig; k++) {
            int base = 1 + (k < 8 ? g1[k] : 0) + (k == g2_pos ? g2 : 0);
            int level = base;
            if (base == (k < 8 ? (k == g2_pos ? 3 : 2) : 1)) {
                // coeff_abs_level_remaining (9.3.3.11)
                int prefix = 0;
                while (prefix < 32 && cabac_bypass(cb)) prefix++;
                int rem;
                if (prefix <= 3) rem = (prefix << rice) + (int)cabac_bypass_bits(cb, rice);
                else rem = (((1 << (prefix - 3)) + 3 - 1) << rice) + (int)cabac_bypass_bits(cb, prefix - 3 + rice);
                level = base + rem;
                if (level > 3 * (1 << rice)) rice = MIN(rice + 1, 4);
            }
            sum += level;
            int neg = (signs >> (nsig - 1 - k)) & 1;
            if (hidden && k == nsig - 1) neg = sum & 1;
            int p = sig[k];
            int xc = (xs << 2) + pos_scan[p][0], yc = (ys << 2) + pos_scan[p][1];
            coef[yc * n + xc] = neg ? -level : level;
            d->nz[nnz++] = (uint16_t)(yc * n + xc);
            if (xc > maxx) maxx = xc;
            if (yc > maxy) maxy = yc;
        }
    }

    // dequantization
    int qp;
    if (c == 0) {
        qp = d->qp_y;
    } else {
        int qpi = CLIP3(0, 57, d->qp_y + (c == 1 ? d->cb_qp_offset : d->cr_qp_offset));
        qp = chroma_qp_table(qpi);
    }
    int16_t *res = d->res;
    if (d->cu_transquant_bypass) {
        for (int i = 0; i < n * n; i++) res[i] = (int16_t)coef[i];
    } else {
        int bd_shift = 8 + log2 - 5;
        int64_t scale = (int64_t)level_scale[qp % 6] << (qp / 6);
        const uint8_t *sf = d->sf[log2 - 2][c];
        for (int i = 0; i < nnz; i++) {
            int k = d->nz[i];
            int64_t t = ((int64_t)coef[k] * sf[k] * scale + (1 << (bd_shift - 1))) >> bd_shift;
            coef[k] = (int32_t)CLIP3(-32768, 32767, t);
        }
        inverse_transform(coef, log2, c == 0 && log2 == 2, maxx, maxy, res);
    }
    for (int i = 0; i < nnz; i++) coef[d->nz[i]] = 0;
    // reconstruction
    uint8_t *dst = d->plane[c] + (size_t)y0 * d->stride[c] + x0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) dst[y * d->stride[c] + x] = clip_pixel(dst[y * d->stride[c] + x] + res[y * n + x]);
    return 0;
}

// ---------------------------------------------------------------------------
// coding tree (7.3.8)

static void fill4(uint8_t *map, int w4, int x0, int y0, int size, uint8_t v) {   // luma coordinates
    for (int y = y0 >> 2; y < (y0 + size) >> 2; y++) memset(map + y * w4 + (x0 >> 2), v, (size_t)(size >> 2));
}

// 7.3.8.3 sao(): parsed (the SAO filter itself is not applied yet)
static void parse_sao(Dec *d, int rx, int ry) {
    Cabac *c = &d->cabac;
    int ctb_addr = ry * d->ctb_w + rx;
    if (rx > 0 && d->slice_addr[ctb_addr - 1] == d->slice_addr_rs && cabac_decision(c, CTX_SAO_MERGE)) return;
    if (ry > 0 && d->slice_addr[ctb_addr - d->ctb_w] == d->slice_addr_rs && cabac_decision(c, CTX_SAO_MERGE)) return;
    int type[3] = {0};
    for (int ci = 0; ci < 3; ci++) {
        if (!(ci == 0 ? d->sao_luma : d->sao_chroma)) continue;
        if (ci == 2) type[2] = type[1];
        else if (cabac_decision(c, CTX_SAO_TYPE)) type[ci] = cabac_bypass(c) ? 2 : 1;
        if (!type[ci]) continue;
        int abs_off[4];
        for (int i = 0; i < 4; i++) {   // TR, cMax 7 (8-bit)
            int v = 0;
            while (v < 7 && cabac_bypass(c)) v++;
            abs_off[i] = v;
        }
        if (type[ci] == 1) {   // band offset
            for (int i = 0; i < 4; i++)
                if (abs_off[i]) cabac_bypass(c);
            cabac_bypass_bits(c, 5);
        } else if (ci < 2) {   // edge offset class (Cr shares Cb's)
            cabac_bypass_bits(c, 2);
        }
    }
}

// the luma QP of a new quantization group at (xq, yq) (8.6.1)
static void qp_group(Dec *d, int xq, int yq) {
    int ctb_mask = ~(d->ctb - 1);
    int qa = d->qp_prev_cu, qb = d->qp_prev_cu;
    if ((xq & ctb_mask) == ((xq - 1) & ctb_mask) && xq > 0) qa = d->qp[(yq >> 2) * d->w4 + ((xq - 1) >> 2)];
    if ((yq & ctb_mask) == ((yq - 1) & ctb_mask) && yq > 0) qb = d->qp[((yq - 1) >> 2) * d->w4 + (xq >> 2)];
    d->qp_pred = (qa + qb + 1) >> 1;
    d->is_cu_qp_delta_coded = 0;
    d->cu_qp_delta = 0;
    d->qp_y = d->qp_pred;
}

// candIntraPredModeX (8.4.2); (cx, cy, cn): the current CU, whose earlier prediction blocks are
// available too (their modes are known before anything of the CU is reconstructed)
static int mpm_neighbour(Dec *d, int x, int y, int y0, int cx, int cy, int cn) {
    int in_cu = x >= cx && y >= cy && x < cx + cn && y < cy + cn;
    if (!in_cu && !avail4(d, x, y, d->slice_addr_rs)) return 1;
    if (y < y0 && (y >> d->log2_ctb) != (y0 >> d->log2_ctb)) return 1;   // above: outside the CTB
    return d->intra_mode[(y >> 2) * d->w4 + (x >> 2)];
}

static int luma_mode(Dec *d, int x0, int y0, int prev_flag, int mpm_idx, int rem, int cx, int cy, int cn) {
    int a = mpm_neighbour(d, x0 - 1, y0, y0, cx, cy, cn), b = mpm_neighbour(d, x0, y0 - 1, y0, cx, cy, cn);
    int cand[3];
    if (a == b) {
        if (a < 2) {
            cand[0] = 0;
            cand[1] = 1;
            cand[2] = 26;
        } else {
            cand[0] = a;
            cand[1] = 2 + ((a + 29) % 32);
            cand[2] = 2 + ((a - 2 + 1) % 32);
        }
    } else {
        cand[0] = a;
        cand[1] = b;
        cand[2] = a != 0 && b != 0 ? 0 : a != 1 && b != 1 ? 1 : 26;
    }
    if (prev_flag) return cand[mpm_idx];
    if (cand[0] > cand[1]) { int t = cand[0]; cand[0] = cand[1]; cand[1] = t; }
    if (cand[0] > cand[2]) { int t = cand[0]; cand[0] = cand[2]; cand[2] = t; }
    if (cand[1] > cand[2]) { int t = cand[1]; cand[1] = cand[2]; cand[2] = t; }
    int mode = rem;
    for (int i = 0; i < 3; i++)
        if (mode >= cand[i]) mode++;
    return mode;
}

// 7.3.8.8 transform_tree / 7.3.8.10 transform_unit, with the reconstruction of each block
static int transform_tree(Dec *d, int x0, int y0, int xbase, int ybase, int log2, int depth, int max_depth,
                          int intra_split, int blk, int parent_cbf_cb, int parent_cbf_cr) {
    Cabac *c = &d->cabac;
    const Sps *s = d->sps;
    int split;
    if (log2 <= s->log2_max_tb && log2 > s->log2_min_tb && depth < max_depth && !(intra_split && depth == 0))
        split = cabac_decision(c, CTX_SPLIT_TRANSFORM + 5 - log2);
    else
        split = log2 > s->log2_max_tb || (intra_split && depth == 0);
    int cbf_cb = 0, cbf_cr = 0;
    if (log2 > 2) {
        if (depth == 0 || parent_cbf_cb) cbf_cb = cabac_decision(c, CTX_CBF_CHROMA + depth);
        if (depth == 0 || parent_cbf_cr) cbf_cr = cabac_decision(c, CTX_CBF_CHROMA + depth);
    } else {   // 4x4 luma: chroma belongs to the parent (decoded with the 4th block)
        cbf_cb = parent_cbf_cb;
        cbf_cr = parent_cbf_cr;
    }
    if (split) {
        int h = 1 << (log2 - 1);
        for (int i = 0; i < 4; i++)
            if (transform_tree(d, x0 + (i & 1) * h, y0 + (i >> 1) * h, x0, y0, log2 - 1, depth + 1, max_depth, intra_split,
                               i, cbf_cb, cbf_cr))
                return -1;
        return 0;
    }
    int cbf_luma = cabac_decision(c, CTX_CBF_LUMA + (depth == 0 ? 1 : 0));   // (always coded: intra)
    int chroma_here = log2 > 2;   // 4:2:0: chroma of 4x4 luma blocks goes with blkIdx 3
    int cbf_chroma = cbf_cb || cbf_cr;   // (4x4 luma: the parent's, for all four blocks)
    if ((cbf_luma || cbf_chroma) && d->pps->cu_qp_delta_enabled && !d->is_cu_qp_delta_coded) {
        // cu_qp_delta_abs: prefix TU (cMax 5, ctx 0 then 1), suffix EG0
        int v = 0;
        while (v < 5 && cabac_decision(c, CTX_CU_QP_DELTA + (v ? 1 : 0))) v++;
        if (v == 5) {
            int k = 0;
            while (k < 32 && cabac_bypass(c)) k++;
            v += (int)(((1u << k) - 1) + cabac_bypass_bits(c, k));
        }
        if (v && cabac_bypass(c)) v = -v;
        d->is_cu_qp_delta_coded = 1;
        d->cu_qp_delta = v;
        d->qp_y = ((d->qp_pred + v + 52) % 52 + 52) % 52;
    }
    // luma: prediction, then the residual
    int mode = d->intra_mode[(y0 >> 2) * d->w4 + (x0 >> 2)];
    intra_predict(d, 0, x0, y0, log2, mode);
    if (cbf_luma && residual(d, x0, y0, log2, 0, mode)) return -1;
    fill4(d->decoded, d->w4, x0, y0, 1 << log2, 1);
    // chroma
    if (chroma_here || blk == 3) {
        int xc = chroma_here ? x0 : xbase, yc = chroma_here ? y0 : ybase;
        int log2c = chroma_here ? log2 - 1 : 2;
        for (int ci = 1; ci <= 2; ci++) {
            intra_predict(d, ci, xc >> 1, yc >> 1, log2c, d->intra_chroma_mode);
            if ((ci == 1 ? cbf_cb : cbf_cr) && residual(d, xc >> 1, yc >> 1, log2c, ci, d->intra_chroma_mode)) return -1;
        }
    }
    return 0;
}

// 7.3.8.5 coding_unit (intra)
static int coding_unit(Dec *d, int x0, int y0, int log2) {
    Cabac *c = &d->cabac;
    const Sps *s = d->sps;
    int n = 1 << log2;
    d->cu_transquant_bypass = d->pps->transquant_bypass ? cabac_decision(c, CTX_TQ_BYPASS) : 0;
    int part_nxn = 0;
    if (log2 == s->log2_min_cb) part_nxn = !cabac_decision(c, CTX_PART_MODE);
    if (part_nxn && log2 <= s->log2_min_tb) return -1;
    if (s->pcm && !part_nxn) return -1;   // (pcm_flag: not handled)
    int nb = part_nxn ? 4 : 1, pb = part_nxn ? n / 2 : n;
    int prev[4];
    for (int i = 0; i < nb; i++) prev[i] = cabac_decision(c, CTX_PREV_INTRA);
    for (int i = 0; i < nb; i++) {
        int xp = x0 + (i & 1) * pb, yp = y0 + (i >> 1) * pb;
        int mpm = 0, rem = 0;
        if (prev[i]) {
            mpm = cabac_bypass(c);
            if (mpm) mpm += cabac_bypass(c);
        } else {
            rem = (int)cabac_bypass_bits(c, 5);
        }
        int mode = luma_mode(d, xp, yp, prev[i], mpm, rem, x0, y0, n);
        fill4(d->intra_mode, d->w4, xp, yp, pb, (uint8_t)mode);
    }
    // chroma mode (8.4.3): 4 -> the luma mode; 0..3 -> planar, vertical, horizontal, DC (34 if equal)
    int cm = 4;
    if (cabac_decision(c, CTX_CHROMA_MODE)) cm = (int)cabac_bypass_bits(c, 2);
    int lm = d->intra_mode[(y0 >> 2) * d->w4 + (x0 >> 2)];
    if (cm == 4) d->intra_chroma_mode = lm;
    else {
        static const int modes[4] = {0, 26, 10, 1};
        d->intra_chroma_mode = modes[cm] == lm ? 34 : modes[cm];
    }
    int max_depth = s->max_transform_hierarchy_depth_intra + part_nxn;
    if (transform_tree(d, x0, y0, x0, y0, log2, 0, max_depth, part_nxn, 0, 0, 0)) return -1;
    // the CU's QP (for the prediction of later quantization groups and the deblocking)
    for (int y = y0 >> 2; y < (y0 + n) >> 2; y++)
        memset(d->qp + y * d->w4 + (x0 >> 2), d->qp_y, (size_t)(n >> 2));
    d->qp_prev_cu = d->qp_y;
    return 0;
}

static int coding_quadtree(Dec *d, int x0, int y0, int log2, int depth) {
    const Sps *s = d->sps;
    int n = 1 << log2, split;
    if (x0 + n <= d->w && y0 + n <= d->h && log2 > s->log2_min_cb) {
        int ctx = 0;
        if (avail4(d, x0 - 1, y0, d->slice_addr_rs) && d->ct_depth[(y0 >> 2) * d->w4 + ((x0 - 1) >> 2)] > depth) ctx++;
        if (avail4(d, x0, y0 - 1, d->slice_addr_rs) && d->ct_depth[((y0 - 1) >> 2) * d->w4 + (x0 >> 2)] > depth) ctx++;
        split = cabac_decision(&d->cabac, CTX_SPLIT_CU + ctx);
    } else {
        split = log2 > s->log2_min_cb;
    }
    if (d->pps->cu_qp_delta_enabled && log2 >= d->log2_ctb - d->pps->diff_cu_qp_delta_depth) qp_group(d, x0, y0);
    if (split) {
        int h = n >> 1;
        for (int i = 0; i < 4; i++) {
            int x = x0 + (i & 1) * h, y = y0 + (i >> 1) * h;
            if (x < d->w && y < d->h && coding_quadtree(d, x, y, log2 - 1, depth + 1)) return -1;
        }
        return 0;
    }
    fill4(d->ct_depth, d->w4, x0, y0, n, (uint8_t)depth);
    return coding_unit(d, x0, y0, log2);
}

// ---------------------------------------------------------------------------
// slices (7.3.6) and the picture

static int decode_slice(Dec *d, const Sps *sps_table, const Pps *pps_table, const uint8_t *nal, size_t len,
                        int *pic_started) {
    Rbsp r;
    if (unescape(nal, len, &r)) return -1;
    int rc = -1;
    Bits b = {r.data, r.len, 16};
    int nal_type = (nal[0] >> 1) & 63;
    int first = (int)bits_u(&b, 1);
    if (nal_type >= 16 && nal_type <= 23) bits_u(&b, 1);   // no_output_of_prior_pics_flag
    uint32_t pps_id = bits_ue(&b);
    if (pps_id > 63 || !pps_table[pps_id].valid) goto out;
    const Pps *pps = &pps_table[pps_id];
    const Sps *sps = &sps_table[pps->sps_id];
    if (!*pic_started) {
        if (!first) goto out;
        // the picture's buffers
        d->sps = sps;
        d->pps = pps;
        d->w = sps->width;
        d->h = sps->height;
        d->log2_ctb = sps->log2_ctb;
        d->ctb = 1 << sps->log2_ctb;
        d->ctb_w = (d->w + d->ctb - 1) >> d->log2_ctb;
        d->ctb_h = (d->h + d->ctb - 1) >> d->log2_ctb;
        int pw = d->ctb_w << d->log2_ctb, ph = d->ctb_h << d->log2_ctb;
        d->stride[0] = pw;
        d->stride[1] = d->stride[2] = pw / 2;
        d->plane[0] = calloc((size_t)pw * ph * 3 / 2, 1);
        if (!d->plane[0]) goto out;
        d->plane[1] = d->plane[0] + (size_t)pw * ph;
        d->plane[2] = d->plane[1] + (size_t)pw * ph / 4;
        d->w4 = pw / 4;
        d->h4 = ph / 4;
        size_t n4 = (size_t)d->w4 * d->h4;
        d->decoded = calloc(n4 * 4, 1);
        d->slice_addr = malloc(sizeof(int) * (size_t)d->ctb_w * d->ctb_h);
        if (!d->decoded || !d->slice_addr) goto out;
        d->intra_mode = d->decoded + n4;
        d->qp = (int8_t *)(d->decoded + 2 * n4);
        d->ct_depth = d->decoded + 3 * n4;
        for (int i = 0; i < d->ctb_w * d->ctb_h; i++) d->slice_addr[i] = -1;
        *pic_started = 1;
    } else if (first || sps != d->sps) {
        goto out;
    }
    if (d->pps != pps || !d->sf_ready) {
        d->pps = pps;
        scaling_factors(d);
        d->sf_ready = 1;
    }
    int pic_ctbs = d->ctb_w * d->ctb_h;
    int addr = 0;
    if (!first) {
        if (pps->dependent_slice_segments && bits_u(&b, 1)) goto out;   // (dependent slices: not handled)
        int bits = 0;
        while ((1 << bits) < pic_ctbs) bits++;
        addr = (int)bits_u(&b, bits);
        if (addr >= pic_ctbs) goto out;
    }
    bits_u(&b, pps->num_extra_slice_header_bits);
    if (bits_ue(&b) != 2) goto out;   // slice_type: I only
    if (pps->output_flag_present) bits_u(&b, 1);
    if (nal_type != 19 && nal_type != 20) {   // not IDR: POC and reference picture sets (unused here)
        bits_u(&b, sps->log2_max_poc_lsb);
        if (!bits_u(&b, 1)) {
            int num_delta[65] = {0};
            if (skip_st_ref_pic_set(&b, sps->num_short_term_ref_pic_sets, sps->num_short_term_ref_pic_sets, num_delta))
                goto out;
        } else if (sps->num_short_term_ref_pic_sets > 1) {
            int bits = 0;
            while ((1 << bits) < sps->num_short_term_ref_pic_sets) bits++;
            bits_u(&b, bits);
        }
        if (sps->long_term_ref_pics_present) goto out;   // (not expected in still pictures)
        if (sps->temporal_mvp) bits_u(&b, 1);
    }
    d->sao_luma = d->sao_chroma = 0;
    if (sps->sao) {
        d->sao_luma = (int)bits_u(&b, 1);
        d->sao_chroma = (int)bits_u(&b, 1);
    }
    d->slice_qp = pps->init_qp + bits_se(&b);
    if (d->slice_qp < 0 || d->slice_qp > 51) goto out;
    d->cb_qp_offset = pps->cb_qp_offset;
    d->cr_qp_offset = pps->cr_qp_offset;
    if (pps->slice_chroma_qp_offsets_present) {
        d->cb_qp_offset += bits_se(&b);
        d->cr_qp_offset += bits_se(&b);
    }
    int deblock_disabled = pps->deblocking_disabled;
    if (pps->deblocking_override_enabled && bits_u(&b, 1)) {
        deblock_disabled = (int)bits_u(&b, 1);
        if (!deblock_disabled) {
            bits_se(&b);
            bits_se(&b);
        }
    }
    if (pps->loop_filter_across_slices && (d->sao_luma || d->sao_chroma || !deblock_disabled)) bits_u(&b, 1);
    uint32_t entry_offsets[1024];
    uint32_t num_entry = 0;
    if (pps->entropy_coding_sync) {
        num_entry = bits_ue(&b);
        if (num_entry > 1023) goto out;
        if (num_entry) {
            uint32_t bits = bits_ue(&b) + 1;
            if (bits > 32) goto out;
            for (uint32_t i = 0; i < num_entry; i++) entry_offsets[i] = (uint32_t)bits_u(&b, (int)bits) + 1;
        }
    }
    if (pps->slice_header_extension_present) {
        uint32_t n = bits_ue(&b);
        for (uint32_t i = 0; i < n; i++) bits_u(&b, 8);
    }
    bits_u(&b, 1);   // alignment_bit_equal_to_one
    size_t data_rbsp = (b.pos + 7) >> 3;
    if (data_rbsp > r.len) goto out;
    // substreams: one per CTB row with WPP (their offsets count the bytes of the NAL unit)
    size_t raw = rbsp_to_raw(&r, data_rbsp);
    size_t sub_start[1025];
    sub_start[0] = data_rbsp;
    for (uint32_t i = 0; i < num_entry; i++) {
        raw += entry_offsets[i];
        if (raw > len) goto out;
        sub_start[i + 1] = raw_to_rbsp(&r, raw);
    }

    d->slice_addr_rs = addr;
    const uint8_t *end = r.data + r.len;
    Cabac *c = &d->cabac;
    cabac_start(c, r.data + data_rbsp, end);
    cabac_init_contexts(c, d->slice_qp);
    uint8_t wpp_state[NUM_CTX];
    int have_wpp_state = 0;
    d->qp_prev_cu = d->qp_y = d->qp_pred = d->slice_qp;
    d->is_cu_qp_delta_coded = 0;
    d->cu_qp_delta = 0;
    uint32_t sub = 0;
    for (int ctb = addr;; ctb++) {
        if (ctb >= pic_ctbs) goto out;
        int rx = ctb % d->ctb_w, ry = ctb / d->ctb_w;
        if (pps->entropy_coding_sync && rx == 0 && ctb != addr) {
            // a new substream: the contexts of the CTB above and to the right (after its 2nd CTB)
            if (++sub > num_entry) goto out;
            cabac_start(c, r.data + sub_start[sub], end);
            if (have_wpp_state && d->slice_addr[(ry - 1) * d->ctb_w + MIN(1, d->ctb_w - 1)] == addr)
                memcpy(c->state, wpp_state, NUM_CTX);
            else
                cabac_init_contexts(c, d->slice_qp);
            d->qp_prev_cu = d->slice_qp;
        }
        d->slice_addr[ctb] = addr;
        int x0 = rx << d->log2_ctb, y0 = ry << d->log2_ctb;
        if (d->sao_luma || d->sao_chroma) parse_sao(d, rx, ry);
        if (!d->pps->cu_qp_delta_enabled) d->qp_y = d->slice_qp;
        if (coding_quadtree(d, x0, y0, d->log2_ctb, 0)) goto out;
        if (pps->entropy_coding_sync && rx == MIN(1, d->ctb_w - 1)) {
            memcpy(wpp_state, c->state, NUM_CTX);
            have_wpp_state = 1;
        }
        if (cabac_terminate(c)) break;   // end_of_slice_segment_flag
    }
    rc = 0;
out:
    rbsp_free(&r);
    return rc;
}

static pthread_once_t tables_once = PTHREAD_ONCE_INIT;
static void init_tables(void) {
    init_scans();
    init_dct();
}

int nhevc_decode(const uint8_t *hvcc, size_t hvcc_len, const uint8_t *data, size_t len, int width, int height,
                 uint8_t *const planes[3], const size_t pitch[3]) {
    pthread_once(&tables_once, init_tables);
    Sps *sps = calloc(16, sizeof(Sps));
    Pps *pps = calloc(64, sizeof(Pps));
    Dec *d = calloc(1, sizeof(Dec));
    int rc = -1, started = 0;
    if (!sps || !pps || !d || hvcc_len < 23) goto out;
    // parameter sets from hvcC
    int len_size = (hvcc[21] & 3) + 1;
    size_t p = 23;
    for (int a = 0; a < hvcc[22]; a++) {
        if (p + 3 > hvcc_len) goto out;
        int type = hvcc[p] & 63, n = hvcc[p + 1] << 8 | hvcc[p + 2];
        p += 3;
        for (int k = 0; k < n; k++) {
            if (p + 2 > hvcc_len) goto out;
            size_t l = (size_t)(hvcc[p] << 8 | hvcc[p + 1]);
            p += 2;
            if (l > hvcc_len - p || l < 3) goto out;
            Rbsp r;
            if (unescape(hvcc + p, l, &r)) goto out;
            int e = 0;
            if (type == 33) e = parse_sps(r.data, r.len, sps);
            else if (type == 34) e = parse_pps(r.data, r.len, sps, pps);
            rbsp_free(&r);
            if (e) goto out;
            p += l;
        }
    }
    // the picture's NAL units
    size_t o = 0;
    while (o + (size_t)len_size <= len) {
        size_t l = 0;
        for (int k = 0; k < len_size; k++) l = l << 8 | data[o + k];
        o += (size_t)len_size;
        if (l > len - o || l < 3) goto out;
        int type = (data[o] >> 1) & 63;
        if (type <= 21) {
            if (type < 16) goto out;   // (only IRAP pictures: IDR, CRA, BLA)
            if (decode_slice(d, sps, pps, data + o, l, &started)) goto out;
        }
        o += l;
    }
    if (!started) goto out;
    {
        const Sps *s = d->sps;
        if (s->chroma_format_idc != 1 || s->bit_depth_luma != 8 || s->bit_depth_chroma != 8) goto out;
        if (s->width - s->conf_left - s->conf_right != width || s->height - s->conf_top - s->conf_bottom != height)
            goto out;
        for (int c = 0; c < 3; c++) {
            int sh = c ? 1 : 0;
            int w = width >> sh, h = height >> sh;
            const uint8_t *src = d->plane[c] + (size_t)(s->conf_top >> sh) * d->stride[c] + (s->conf_left >> sh);
            for (int y = 0; y < h; y++) memcpy(planes[c] + (size_t)y * pitch[c], src + (size_t)y * d->stride[c], (size_t)w);
        }
    }
    rc = 0;
out:
    if (d) {
        free(d->plane[0]);
        free(d->decoded);
        free(d->slice_addr);
    }
    free(d);
    free(sps);
    free(pps);
    return rc;
}
