// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro_prefetch: see nitro_prefetch.h
#include "nitro_prefetch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct NvLoader {
    NvLoaderOps ops;
    void **files;
    int nfiles;
    struct { int index; void *image; unsigned serial; } cache[NV_CACHE];
    int ncache;
    char **failed;         // per file: NULL, or why it could not be decoded ("": no reason given)
    int cur, dir;
    unsigned gen;          // bumped when the file list changes: in-flight decodes are dropped
    unsigned nserial;      // the last serial given
    void **grave;          // evicted images, until the main thread reaps them
    int ngrave, gcap;
    int quit;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    pthread_t thread;
};

static void bury_locked(NvLoader *L, void *image) {
    if (!image) return;
    if (L->ngrave == L->gcap) {
        void **g = realloc(L->grave, (size_t)(L->gcap ? L->gcap * 2 : 64) * sizeof *g);
        if (!g) {   // (out of memory: the image leaks, which is the lesser evil)
            fprintf(stderr, "nitro_prefetch: out of memory\n");
            return;
        }
        L->grave = g;
        L->gcap = L->gcap ? L->gcap * 2 : 64;
    }
    L->grave[L->ngrave++] = image;
}

static void drop_locked(NvLoader *L, int c) {   // cache entry c out, evicted
    bury_locked(L, L->cache[c].image);
    L->cache[c] = L->cache[--L->ncache];
}

static void failed_set(NvLoader *L, int i, char *error) {
    free(L->failed[i]);
    L->failed[i] = error;
}

// Priority order: current, then NV_AHEAD in the paging direction, then NV_BEHIND.
static int wanted_locked(const NvLoader *L, int *w) {
    int n = 0;
#define ADD(i) do { int _i = (i); int dup = 0; for (int q = 0; q < n; q++) dup |= w[q] == _i; \
                    if (_i >= 0 && _i < L->nfiles && !dup) w[n++] = _i; } while (0)
    ADD(L->cur);
    for (int k = 1; k <= MAX(NV_AHEAD, NV_BEHIND); k++) {
        if (k <= NV_AHEAD) ADD(L->cur + L->dir * k);
        if (k <= NV_BEHIND) ADD(L->cur - L->dir * k);
    }
#undef ADD
    return n;
}

static void *run(void *arg) {
    NvLoader *L = arg;
    if (L->ops.thread_started) L->ops.thread_started(L->ops.ctx);
    for (;;) {
        pthread_mutex_lock(&L->lock);
        int w[2 * MAX(NV_AHEAD, NV_BEHIND) + 2], nw, next = -1;
        while (!L->quit) {
            nw = wanted_locked(L, w);
            for (int k = 0; k < nw && next < 0; k++) {
                int have = 0;
                for (int c = 0; c < L->ncache; c++) have |= L->cache[c].index == w[k];
                if (!have && !L->failed[w[k]]) next = w[k];
            }
            if (next >= 0) break;
            pthread_cond_wait(&L->cv, &L->lock);
        }
        if (L->quit) {
            pthread_mutex_unlock(&L->lock);
            return NULL;
        }
        for (int c = 0; c < L->ncache;) {   // evict everything outside the window
            int in = 0;
            for (int k = 0; k < nw; k++) in |= L->cache[c].index == w[k];
            if (in) c++;
            else drop_locked(L, c);
        }
        void *path = L->ops.path_dup(L->files[next], L->ops.ctx);
        unsigned gen = L->gen;
        pthread_mutex_unlock(&L->lock);
        if (!path) continue;   // (out of memory: tried again)

        char *error = NULL;
        void *image = L->ops.decode(path, &error, L->ops.ctx);

        pthread_mutex_lock(&L->lock);
        // the file list (index no longer valid) or the settings (a DNG's exposure) changed meanwhile
        int stale = gen != L->gen || L->quit || (image && L->ops.stale && L->ops.stale(image, L->ops.ctx));
        if (stale || (image && L->ncache == NV_CACHE)) {
            bury_locked(L, image);
        } else if (image) {
            L->cache[L->ncache].index = next;
            L->cache[L->ncache].image = image;
            L->cache[L->ncache].serial = ++L->nserial;
            L->ncache++;
        } else {
            failed_set(L, next, error ? error : strdup(""));
            error = NULL;
        }
        int quit = L->quit;
        pthread_mutex_unlock(&L->lock);
        free(error);
        if (!stale && !image) {
            const char *name = L->ops.path_name ? L->ops.path_name(path, L->ops.ctx) : NULL;
            fprintf(stderr, "cannot decode %s\n", name ? name : "?");
        }
        L->ops.path_free(path, L->ops.ctx);
        if (quit) return NULL;
        L->ops.notify(stale ? -1 : next, L->ops.ctx);
    }
}

NvLoader *nv_loader_new(const NvLoaderOps *ops, void **files, int n) {
    NvLoader *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->ops = *ops;
    L->files = files;
    L->nfiles = n;
    L->failed = calloc((size_t)MAX(n, 1), sizeof *L->failed);
    L->dir = 1;
    if (!L->failed || pthread_mutex_init(&L->lock, NULL)) {
        free(L->failed);
        free(L);
        return NULL;
    }
    pthread_cond_init(&L->cv, NULL);
    if (pthread_create(&L->thread, NULL, run, L)) {
        pthread_cond_destroy(&L->cv);
        pthread_mutex_destroy(&L->lock);
        free(L->failed);
        free(L);
        return NULL;
    }
    return L;
}

void nv_loader_stop(NvLoader *L) {
    if (!L) return;
    pthread_mutex_lock(&L->lock);
    L->quit = 1;
    pthread_cond_broadcast(&L->cv);
    pthread_mutex_unlock(&L->lock);
    pthread_join(L->thread, NULL);   // (waits for a decode in progress)
    for (int c = 0; c < L->ncache; c++) L->ops.free_image(L->cache[c].image, L->ops.ctx);
    for (int i = 0; i < L->ngrave; i++) L->ops.free_image(L->grave[i], L->ops.ctx);
    for (int i = 0; i < L->nfiles; i++) {
        L->ops.path_free(L->files[i], L->ops.ctx);
        free(L->failed[i]);
    }
    free(L->files);
    free(L->failed);
    free(L->grave);
    pthread_cond_destroy(&L->cv);
    pthread_mutex_destroy(&L->lock);
    free(L);
}

void *nv_loader_get(NvLoader *L, int i, unsigned *serial) {
    void *image = NULL;
    pthread_mutex_lock(&L->lock);
    for (int c = 0; c < L->ncache; c++)
        if (L->cache[c].index == i) {
            image = L->cache[c].image;
            if (serial) *serial = L->cache[c].serial;
        }
    pthread_mutex_unlock(&L->lock);
    return image;
}

int nv_loader_failed(NvLoader *L, int i, char **error) {
    pthread_mutex_lock(&L->lock);
    const char *f = i >= 0 && i < L->nfiles ? L->failed[i] : NULL;
    if (error) *error = f && *f ? strdup(f) : NULL;
    pthread_mutex_unlock(&L->lock);
    return f != NULL;
}

void nv_loader_focus(NvLoader *L, int cur, int dir) {
    pthread_mutex_lock(&L->lock);
    L->cur = cur;
    L->dir = dir < 0 ? -1 : 1;
    pthread_cond_signal(&L->cv);
    pthread_mutex_unlock(&L->lock);
}

void nv_loader_replace(NvLoader *L, void **files, int n, int old_index, int new_index) {
    pthread_mutex_lock(&L->lock);
    int keep = -1;
    for (int c = 0; c < L->ncache; c++) {
        if (old_index >= 0 && L->cache[c].index == old_index) keep = c;
        else bury_locked(L, L->cache[c].image);
    }
    if (keep >= 0) {
        L->cache[0] = L->cache[keep];
        L->cache[0].index = new_index;
        L->ncache = 1;
    } else {
        L->ncache = 0;
    }
    char *keep_failed = old_index >= 0 && old_index < L->nfiles ? L->failed[old_index] : NULL;
    if (keep_failed) L->failed[old_index] = NULL;
    for (int i = 0; i < L->nfiles; i++) {
        L->ops.path_free(L->files[i], L->ops.ctx);
        free(L->failed[i]);
    }
    free(L->files);
    free(L->failed);
    L->files = files;
    L->nfiles = n;
    L->failed = calloc((size_t)MAX(n, 1), sizeof *L->failed);
    if (L->failed && keep_failed && new_index >= 0 && new_index < n) L->failed[new_index] = keep_failed;
    else free(keep_failed);
    if (!L->failed) {   // (out of memory: nothing can be decoded; the viewer shows "loading")
        fprintf(stderr, "nitro_prefetch: out of memory\n");
        L->nfiles = 0;
    }
    L->cur = new_index >= 0 ? new_index : 0;
    L->gen++;
    pthread_cond_signal(&L->cv);
    pthread_mutex_unlock(&L->lock);
}

void nv_loader_remove(NvLoader *L, int i) {
    pthread_mutex_lock(&L->lock);
    if (i < 0 || i >= L->nfiles) {
        pthread_mutex_unlock(&L->lock);
        return;
    }
    for (int c = 0; c < L->ncache;) {
        if (L->cache[c].index == i) {
            drop_locked(L, c);
            continue;
        }
        if (L->cache[c].index > i) L->cache[c].index--;
        c++;
    }
    L->ops.path_free(L->files[i], L->ops.ctx);
    free(L->failed[i]);
    memmove(L->files + i, L->files + i + 1, (size_t)(L->nfiles - i - 1) * sizeof *L->files);
    memmove(L->failed + i, L->failed + i + 1, (size_t)(L->nfiles - i - 1) * sizeof *L->failed);
    L->nfiles--;
    L->gen++;   // (a decode in flight would land at the old index)
    pthread_cond_signal(&L->cv);
    pthread_mutex_unlock(&L->lock);
}

int nv_loader_insert(NvLoader *L, int i, void *path) {
    pthread_mutex_lock(&L->lock);
    if (i < 0 || i > L->nfiles) {
        pthread_mutex_unlock(&L->lock);
        return -1;
    }
    void **f = realloc(L->files, (size_t)(L->nfiles + 1) * sizeof *f);
    if (f) L->files = f;
    char **fl = f ? realloc(L->failed, (size_t)(L->nfiles + 1) * sizeof *fl) : NULL;
    if (fl) L->failed = fl;
    if (!f || !fl) {
        pthread_mutex_unlock(&L->lock);
        return -1;
    }
    for (int c = 0; c < L->ncache; c++)
        if (L->cache[c].index >= i) L->cache[c].index++;
    memmove(L->files + i + 1, L->files + i, (size_t)(L->nfiles - i) * sizeof *L->files);
    memmove(L->failed + i + 1, L->failed + i, (size_t)(L->nfiles - i) * sizeof *L->failed);
    L->files[i] = path;
    L->failed[i] = NULL;
    L->nfiles++;
    L->gen++;
    pthread_cond_signal(&L->cv);
    pthread_mutex_unlock(&L->lock);
    return 0;
}

void **nv_loader_files(NvLoader *L, int *n) {
    pthread_mutex_lock(&L->lock);
    void **f = L->files;
    if (n) *n = L->nfiles;
    pthread_mutex_unlock(&L->lock);
    return f;
}

int nv_loader_replace_image(NvLoader *L, unsigned serial, void *image) {
    int at = -1;
    if (!image) return -1;
    pthread_mutex_lock(&L->lock);
    int stale = L->ops.stale && L->ops.stale(image, L->ops.ctx);   // (outdated by a later key press)
    for (int c = 0; c < L->ncache && !stale; c++)
        if (L->cache[c].serial == serial) {
            bury_locked(L, L->cache[c].image);
            L->cache[c].image = image;
            L->cache[c].serial = ++L->nserial;
            at = L->cache[c].index;
            break;
        }
    if (at < 0) bury_locked(L, image);
    pthread_mutex_unlock(&L->lock);
    return at;
}

int nv_loader_drop(NvLoader *L, int (*pred)(const void *image, void *ctx), void *ctx, int keep_current,
                   unsigned *cur_serial, void **cur_path) {
    int current = 0;
    pthread_mutex_lock(&L->lock);
    for (int c = 0; c < L->ncache;) {
        if (!pred(L->cache[c].image, ctx)) {
            c++;
        } else if (keep_current && L->cache[c].index == L->cur) {
            current = 1;
            if (cur_serial) *cur_serial = L->cache[c].serial;
            if (cur_path) *cur_path = L->ops.path_dup(L->files[L->cur], L->ops.ctx);
            c++;
        } else {
            drop_locked(L, c);
        }
    }
    pthread_cond_signal(&L->cv);
    pthread_mutex_unlock(&L->lock);
    return current;
}

int nv_loader_reap(NvLoader *L, void **dead, int max) {
    pthread_mutex_lock(&L->lock);
    int n = L->ngrave < max ? L->ngrave : max;
    memcpy(dead, L->grave + L->ngrave - n, (size_t)n * sizeof *dead);
    L->ngrave -= n;
    pthread_mutex_unlock(&L->lock);
    return n;
}
