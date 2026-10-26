// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Portable benchmark of the three decoders (macOS, Linux, ...): JPEG, PNG and
// PSD/PSB files by content, each read into memory first, then decoded RUNS
// times into the same output buffer; best and average per file.
//   nbench [-r runs] [-j threads] [-1] [-q] files or directories...
//     -r  decodes per file (default 5)
//     -j  threads (default: all CPUs)
//     -1  also measure single-threaded
//     -q  totals only
// Build (Linux): clang -O3 -march=native -fblocks bench/nbench.c src/nitrojpeg.c src/nitropng.c
//                src/nitropsd.c -lpthread -o nbench
#include "../src/nitro_os.h"
#include "../src/nitrojpeg.h"
#include "../src/nitropng.h"
#include "../src/nitropsd.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
int _dowildcard = -1;   // MinGW: the C runtime expands *.jpg in the arguments (cmd.exe doesn't)
#endif

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The files to measure: arguments as they are, directories replaced by the files in them (sorted).
static char **g_files;
static int g_nfiles, g_cap;
static void add_file(char *path) {
    if (g_nfiles == g_cap) g_files = realloc(g_files, (g_cap = g_cap ? g_cap * 2 : 256) * sizeof *g_files);
    g_files[g_nfiles++] = path;
}
static void add_arg(const char *arg) {
    struct stat st;
    DIR *dir = stat(arg, &st) == 0 && S_ISDIR(st.st_mode) ? opendir(arg) : NULL;
    if (!dir) { add_file(strdup(arg)); return; }
    int first = g_nfiles;
    size_t al = strlen(arg);
    int sep = al && (arg[al - 1] == '/' || arg[al - 1] == '\\');
    for (struct dirent *e; (e = readdir(dir));) {
        if (e->d_name[0] == '.') continue;
        char *p = malloc(al + strlen(e->d_name) + 2);
        sprintf(p, "%s%s%s", arg, sep ? "" : "/", e->d_name);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) add_file(p);
        else free(p);
    }
    closedir(dir);
    qsort(g_files + first, (size_t)(g_nfiles - first), sizeof *g_files, cmp_str);
}

enum { T_JPEG, T_PNG, T_PSD, T_N };
static const char *tname[T_N] = {"JPEG", "PNG", "PSD"};

typedef struct {
    int type, w, h;
    const char *mode;
    double best, avg, best1;   // ms; best1: single thread
} Result;

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n + 64) : NULL;   // decoders may read a little ahead
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) memset(d + n, 0, 64);
    *len = (size_t)n;
    return d;
}

// One decode; returns ms or < 0 on failure.
typedef struct {
    int type;
    const uint8_t *d;
    size_t len;
    nj_info ji;
    np_info pi;
    ps_info si;
    uint8_t *out, *planes[3];
    size_t pitch[3];
} Job;

static double run_once(Job *j, int nthreads, const char **mode) {
    double t0 = nitro_now_ms();
    int rc;
    if (j->type == T_JPEG) {
        nj_stats st;
        rc = nj_decode_planes(j->d, j->len, &j->ji, j->planes, j->pitch, nthreads, &st);
        if (mode) *mode = st.mode == 1 ? "RST" : st.mode == 2 ? "split" : st.mode == 3 ? "prog" : "single";
    } else if (j->type == T_PNG) {
        np_stats st;
        rc = np_decode(j->d, j->len, &j->pi, j->out, nthreads, &st);
        if (mode) *mode = st.mode ? "parallel" : "sequential";
    } else {
        ps_stats st;
        rc = ps_decode(j->d, j->len, &j->si, j->out, nthreads, &st);
        static const char *cn[4] = {"raw", "RLE", "ZIP", "ZIP+pred"};
        if (mode) *mode = cn[j->si.compression & 3];
    }
    double t = nitro_now_ms() - t0;
    return rc ? -1 : t;
}

// Sets up the job (output buffer); 0 if the file is one the decoders handle.
static int prepare(Job *j, const uint8_t *d, size_t len, int *w, int *h) {
    memset(j, 0, sizeof *j);
    j->d = d;
    j->len = len;
    size_t need = 0;
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        if (nj_read_info(d, len, &j->ji) || !j->ji.supported) return -1;
        j->type = T_JPEG;
        size_t off[3];
        for (int c = 0; c < j->ji.ncomp; c++) {
            j->pitch[c] = (size_t)j->ji.plane_w[c];
            off[c] = need;
            need += j->pitch[c] * (size_t)j->ji.plane_h[c];
        }
        if (!(j->out = malloc(need))) return -1;
        for (int c = 0; c < j->ji.ncomp; c++) j->planes[c] = j->out + off[c];
        *w = j->ji.width;
        *h = j->ji.height;
    } else if (len >= 8 && !memcmp(d, "\x89PNG", 4)) {
        if (np_read_info(d, len, &j->pi) || !j->pi.supported) return -1;
        j->type = T_PNG;
        if (!(j->out = malloc(j->pi.raw_size))) return -1;
        *w = j->pi.width;
        *h = j->pi.height;
    } else if (len >= 4 && !memcmp(d, "8BPS", 4)) {
        if (ps_read_info(d, len, &j->si) || !j->si.supported) return -1;
        j->type = T_PSD;
        need = j->si.plane_size * (size_t)(j->si.ncolor + j->si.alpha);
        if (!(j->out = malloc(need))) return -1;
        *w = j->si.width;
        *h = j->si.height;
    } else {
        return -1;
    }
    memset(j->out, 0, j->type == T_PNG ? j->pi.raw_size : need);   // fault the pages in before timing
    return 0;
}

int main(int argc, char **argv) {
    int runs = 5, nthreads = 0, single = 0, quiet = 0, first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (!strcmp(argv[first], "-r") && first + 1 < argc) runs = atoi(argv[++first]);
        else if (!strcmp(argv[first], "-j") && first + 1 < argc) nthreads = atoi(argv[++first]);
        else if (!strcmp(argv[first], "-1")) single = 1;
        else if (!strcmp(argv[first], "-q")) quiet = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[first]); return 2; }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: nbench [-r runs] [-j threads] [-1] [-q] files or directories...   (JPEG, PNG, PSD/PSB)\n");
        return 2;
    }
    if (runs < 1) runs = 1;
    if (nthreads > 0) nj_set_max_workers(nthreads);
    char thr[16];
    snprintf(thr, sizeof thr, "%d", nthreads);
    printf("%d CPUs, %s threads, %d runs per file%s\n\n", nitro_ncpu(), nthreads > 0 ? thr : "all", runs,
           single ? " (+ single-threaded)" : "");   // (-q: this blank line separates the totals)
    double sum_best[T_N] = {0}, sum_avg[T_N] = {0}, sum_best1[T_N] = {0}, mpix[T_N] = {0};
    int count[T_N] = {0}, skipped = 0, failed = 0;
    for (int i = first; i < argc; i++) add_arg(argv[i]);
    for (int i = 0; i < g_nfiles; i++) {
        const char *path = g_files[i], *name = path;
        for (const char *c = path; *c; c++)
            if (*c == '/' || *c == '\\') name = c + 1;
        size_t len;
        uint8_t *d = read_all(path, &len);
        Job j;
        int w = 0, h = 0;
        if (!d || prepare(&j, d, len, &w, &h)) {
            if (!quiet) printf("%-34.34s skipped (not a JPEG / PNG / PSD the decoders handle)\n", name);
            skipped++;
            free(d);
            continue;
        }
        const char *mode = "";
        Result r = {j.type, w, h, "", 1e30, 0, 0};
        int ok = run_once(&j, nthreads, &mode) >= 0;   // warm-up (buffer pools, caches)
        for (int k = 0; ok && k < runs; k++) {
            double t = run_once(&j, nthreads, NULL);
            if (t < 0) { ok = 0; break; }
            if (t < r.best) r.best = t;
            r.avg += t / runs;
        }
        if (ok && single) {
            r.best1 = 1e30;
            for (int k = 0; ok && k < (runs < 3 ? runs : 3); k++) {
                double t = run_once(&j, 1, NULL);
                if (t < 0) ok = 0;
                else if (t < r.best1) r.best1 = t;
            }
        }
        if (!ok) {
            printf("%-34.34s FAILED to decode\n", name);
            failed++;
        } else {
            int t = r.type;
            sum_best[t] += r.best;
            sum_avg[t] += r.avg;
            sum_best1[t] += r.best1;
            mpix[t] += (double)w * h / 1e6;
            count[t]++;
            if (!quiet) {
                printf("%-34.34s %-4s %5dx%-5d %-10s best %7.1f  avg %7.1f ms", name, tname[t], w, h, mode, r.best, r.avg);
                if (single) printf("   1 thread %7.1f ms (x%.1f)", r.best1, r.best1 / r.best);
                printf("\n");
            }
        }
        free(j.out);
        free(d);
    }
    if (!quiet) printf("\n");
    for (int t = 0; t < T_N; t++) {
        if (!count[t]) continue;
        printf("%-4s %3d files  best %7.1f  avg %7.1f ms/image  (%.0f Mpixel/s)", tname[t], count[t],
               sum_best[t] / count[t], sum_avg[t] / count[t], mpix[t] / sum_best[t] * 1e3);
        if (single) printf("   1 thread %7.1f ms/image (x%.1f)", sum_best1[t] / count[t], sum_best1[t] / sum_best[t]);
        printf("\n");
    }
    if (skipped || failed) printf("%d skipped, %d failed\n", skipped, failed);
    return failed != 0;
}
