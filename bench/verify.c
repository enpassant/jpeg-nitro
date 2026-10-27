// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Checks that nitrojpeg's parallel output is bit-identical to a
// single-threaded libjpeg-turbo decode, and prints the split statistics.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include <setjmp.h>
#include <jpeglib.h>
#include "../src/nitrojpeg.h"

static uint8_t *load(const char *fn, size_t *n) {
    FILE *f = fopen(fn, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}

static long hist[8];          // sync distance histogram in MCUs
static const char *hname[8] = {"0", "1", "2", "3-4", "5-8", "9-16", "17+", "never"};
static long nthr, tot_mcus, max_mcus, tot_bits, max_bits;

static void sync_summary(const nj_stats *st, char *out, size_t n) {
    if (st->mode != 2 || !st->nsync) { snprintf(out, n, "-"); return; }
    int mx = 0, never = 0; long sum = 0; uint32_t mb = 0; long sb = 0;
    for (int i = 0; i < st->nsync; i++) {
        int m = st->sync_mcus[i];
        int b = m < 0 ? 7 : m == 0 ? 0 : m == 1 ? 1 : m == 2 ? 2 : m <= 4 ? 3 : m <= 8 ? 4 : m <= 16 ? 5 : 6;
        hist[b]++;
        if (m < 0) { never++; continue; }
        nthr++; sum += m; tot_mcus += m; sb += st->sync_bits[i]; tot_bits += st->sync_bits[i];
        if (m > mx) mx = m;
        if (m > max_mcus) max_mcus = m;
        if (st->sync_bits[i] > mb) mb = st->sync_bits[i];
        if (st->sync_bits[i] > max_bits) max_bits = st->sync_bits[i];
    }
    int ok = st->nsync - never;
    snprintf(out, n, "sync %2d thr: avg %4.1f max %3d MCU, avg %5.0f max %5u bit%s", st->nsync,
             ok ? (double)sum / ok : 0, mx, ok ? (double)sb / ok : 0, mb, never ? " NEVER!" : "");
}

// SOF2 before the first scan?
static int is_progressive(const uint8_t *d, size_t n) {
    for (size_t i = 2; i + 4 <= n && d[i] == 0xFF; i += 2 + ((d[i + 2] << 8) | d[i + 3])) {
        if (d[i + 1] == 0xC2) return 1;
        if (d[i + 1] == 0xDA) break;
    }
    return 0;
}

// libjpeg-turbo's Y/Cb/Cr planes without block smoothing (it smooths progressive files that
// lack the last scans, nitrojpeg doesn't; complete files are the same either way).
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } ErrJ;
static void err_exit(j_common_ptr c) { longjmp(((ErrJ *)c->err)->jb, 1); }
static void err_emit(j_common_ptr c, int level) { (void)c; (void)level; }
static void err_out(j_common_ptr c) { (void)c; }
static int ljt_planes(const uint8_t *d, size_t n, uint8_t *const pl[3], const size_t pitch[3]) {
    struct jpeg_decompress_struct c;
    ErrJ e;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = err_exit;
    e.pub.emit_message = err_emit;
    e.pub.output_message = err_out;
    if (setjmp(e.jb)) { jpeg_destroy_decompress(&c); return -1; }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, d, (unsigned long)n);
    jpeg_read_header(&c, TRUE);
    c.raw_data_out = TRUE;
    c.do_block_smoothing = FALSE;
    c.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&c);
    const int rows = c.max_v_samp_factor * DCTSIZE;
    JSAMPROW r[3][4 * DCTSIZE];
    JSAMPARRAY a[3] = {r[0], r[1], r[2]};
    while (c.output_scanline < c.output_height) {
        size_t mcu = c.output_scanline / rows;
        for (int ci = 0; ci < c.num_components; ci++) {
            int v = c.comp_info[ci].v_samp_factor * DCTSIZE;
            for (int y = 0; y < v; y++) r[ci][y] = pl[ci] + (mcu * v + y) * pitch[ci];
        }
        jpeg_read_raw_data(&c, a, rows);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return 0;
}

// Progressive: nitrojpeg (all threads and 1 thread) vs libjpeg-turbo's Y/Cb/Cr planes (the
// old engine can't decode them), visible part. Returns 1 if they differ.
static int verify_progressive(const char *nm, const uint8_t *d, size_t n, const nj_info *fi) {
    size_t pitch[3], tot = 0;
    uint8_t *pl[3][3];
    for (int c = 0; c < fi->ncomp; c++) { pitch[c] = (fi->plane_w[c] + 255) & ~255; tot += pitch[c] * (fi->plane_h[c] + 16); }
    uint8_t *buf[3];
    for (int k = 0; k < 3; k++) {
        uint8_t *p = buf[k] = calloc(1, tot);
        for (int c = 0; c < fi->ncomp; c++) { pl[k][c] = p; p += pitch[c] * (fi->plane_h[c] + 16); }
    }
    nj_stats st;
    int r1 = nj_decode_planes(d, n, fi, pl[0], pitch, 0, &st);
    int r2 = nj_decode_planes(d, n, fi, pl[1], pitch, 1, NULL);
    int r3 = ljt_planes(d, n, pl[2], pitch);
    int ok = !r1 && !r2 && !r3;
    for (int c = 0; c < fi->ncomp && ok; c++) {
        int vw = (fi->width * fi->h[c] + fi->hmax - 1) / fi->hmax, vh = (fi->height * fi->v[c] + fi->vmax - 1) / fi->vmax;
        for (int y = 0; y < vh && ok; y++)
            for (int k = 0; k < 2; k++)
                if (memcmp(pl[k][c] + (size_t)y * pitch[c], pl[2][c] + (size_t)y * pitch[c], (size_t)vw)) ok = 0;
    }
    if (r1 && r2)
        printf("%-24s %5dx%-5d prog   not decoded (cut off or damaged: the viewer falls back to TurboJPEG)\n", nm, fi->width, fi->height);
    else
        printf("%-24s %5dx%-5d prog   %3d scans  dec %5.1f ms | planes %s\n", nm, fi->width, fi->height, st.bands, st.decode_ms,
               ok ? "OK" : "MISMATCH");
    for (int k = 0; k < 3; k++) free(buf[k]);
    return (r1 && r2) ? 0 : !ok;
}

int main(int argc, char **argv) {
    int bad = 0;
    if (getenv("NJ_SEQ")) nj_set_sequential_scan(atoi(getenv("NJ_SEQ")));
    int engine = getenv("NJ_ENGINE") ? atoi(getenv("NJ_ENGINE")) : 0;
    nj_set_engine(engine);
    if (getenv("NJ_SCALAR")) nj_set_scalar_idct(1);
    for (int i = 1; i < argc; i++) {
        size_t n; uint8_t *d = load(argv[i], &n);
        nj_info fi;
        if (!d || nj_read_info(d, n, &fi) || !fi.supported) { printf("%s: unsupported\n", argv[i]); continue; }
        if (is_progressive(d, n)) {
            const char *nm = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
            bad += verify_progressive(nm, d, n, &fi);
            free(d);
            continue;
        }
        // planar: parallel vs 1 thread
        size_t pitch[3], tot = 0; uint8_t *pa[3], *pb[3];
        for (int c = 0; c < fi.ncomp; c++) { pitch[c] = (fi.plane_w[c] + 255) & ~255; tot += pitch[c] * fi.plane_h[c]; }
        uint8_t *A = calloc(1, tot), *B = calloc(1, tot), *p = A, *q = B;
        for (int c = 0; c < fi.ncomp; c++) { pa[c] = p; pb[c] = q; p += pitch[c] * fi.plane_h[c]; q += pitch[c] * fi.plane_h[c]; }
        nj_stats st;
        int r1 = nj_decode_planes(d, n, &fi, pa, pitch, 0, &st);
        nj_set_engine(1);   // reference: libjpeg-turbo, single thread
        int r2 = nj_decode_planes(d, n, &fi, pb, pitch, 1, NULL);
        nj_set_engine(engine);
        // compare the visible part of each plane: blocks entirely outside the image (partial
        // last MCU row / column) are left undecoded by libjpeg-turbo, their content is undefined
        int planes_ok = !r1 && !r2;
        for (int c = 0; c < fi.ncomp && planes_ok; c++) {
            int vw = (fi.width * fi.h[c] + fi.hmax - 1) / fi.hmax, vh = (fi.height * fi.v[c] + fi.vmax - 1) / fi.vmax;
            for (int y = 0; y < vh && planes_ok; y++)
                if (memcmp(pa[c] + (size_t)y * pitch[c], pb[c] + (size_t)y * pitch[c], (size_t)vw)) planes_ok = 0;
        }
        // BGRX: parallel vs TurboJPEG
        size_t sz = (size_t)fi.width * fi.height * 4;
        uint8_t *C = malloc(sz), *D = malloc(sz);
        int r3 = nj_decode_bgrx(d, n, &fi, C, fi.width * 4, 0, NULL);
        tjhandle h = tj3Init(TJINIT_DECOMPRESS);
        int r4 = tj3Decompress8(h, d, n, D, fi.width * 4, TJPF_BGRX);
        tj3Destroy(h);
        size_t diff = 0; int maxd = 0;
        for (size_t k = 0; k < sz; k++) if ((k & 3) != 3 && C[k] != D[k]) { diff++; int e = abs(C[k] - D[k]); if (e > maxd) maxd = e; }
        char ss[160];
        sync_summary(&st, ss, sizeof ss);
        const char *nm = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        printf("%-24s %5dx%-5d %s %3d bands  scan %4.1f sync %4.1f dec %5.1f ms | %s | planes %s bgrx %s\n",
               nm, fi.width, fi.height, st.mode == 2 ? "split" : st.mode == 1 ? "RST  " : "single", st.bands,
               st.scan_ms, st.sync_ms, st.decode_ms, ss,
               planes_ok ? "OK" : "MISMATCH", (!r3 && !r4 && !diff) ? "OK" : "DIFF");
        if (!planes_ok || r3) bad++;
        free(A); free(B); free(C); free(D); free(d);
    }
    if (nthr || hist[7]) {
        printf("\nsynchronisation over all images (%ld threads):\n", nthr + hist[7]);
        for (int k = 0; k < 8; k++)
            printf("  %-6s MCU: %6ld  (%5.1f%%)\n", hname[k], hist[k], 100.0 * hist[k] / (nthr + hist[7]));
        printf("  avg %.2f MCU / %.0f bits, max %ld MCU / %ld bits\n\n", (double)tot_mcus / nthr,
               (double)tot_bits / nthr, max_mcus, max_bits);
    }
    printf(bad ? "FAILURES: %d\n" : "all OK\n", bad);
    return bad != 0;
}
