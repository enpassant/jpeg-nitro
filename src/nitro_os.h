// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_os.h: the little the decoders need from the OS - a parallel loop, a
// clock, the CPU count, yield. Header-only, included by every decoder (nitrojpeg,
// nitropng, nitropsd, nitroheic / nitrohevc, nitrodng, nitrowebp, nitrotiff,
// nitroavif) and the GNOME viewer's converter.
//
//   macOS:       Grand Central Dispatch (dispatch_apply).
//   elsewhere:   a pthread pool, started on first use (Linux, MinGW, other POSIX).
//
// The loop bodies are blocks (^(size_t i) { ... }): compile with clang, and
// outside macOS add -fblocks. The blocks are only called while nitro_parallel()
// runs (never copied), so no blocks runtime library is needed (see below).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef __BLOCKS__
#error "nitrojpeg / nitropng / nitropsd use blocks: compile with clang -fblocks"
#endif

typedef void (^nitro_body)(size_t);

#if defined(__APPLE__) && !defined(NITRO_PTHREAD_POOL)
// ---------------------------------------------------------------------------
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <unistd.h>

// body(0) ... body(n-1) in parallel; returns when all are done.
static inline void nitro_parallel(size_t n, nitro_body body) {
    dispatch_apply(n, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), body);
}

static inline double nitro_now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

#else
// ---------------------------------------------------------------------------
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#if !defined(__APPLE__) || defined(NITRO_BLOCK_STUBS)
// Stack / global blocks point to these, normally defined by the blocks runtime.
// Nothing else of the runtime is needed as long as the decoders don't use
// __block variables or blocks that capture blocks (those need
// _Block_object_assign / _Block_object_dispose): a block changes a local
// through a pointer instead. One definition per program: COMDAT on Windows,
// weak elsewhere, so a real blocks runtime (libBlocksRuntime) still links.
#ifdef _WIN32
#define NITRO_ONE_DEF __attribute__((selectany))
#else
#define NITRO_ONE_DEF __attribute__((weak))
#endif
NITRO_ONE_DEF void *_NSConcreteStackBlock[32] = {0};
NITRO_ONE_DEF void *_NSConcreteGlobalBlock[32] = {0};
#endif

// One pool per decoder file (static): workers sleep on a condition variable
// between loops. Calls from several threads at once take turns; a call from
// inside a loop body runs sequentially.
static struct {
    pthread_mutex_t mu, submit;
    pthread_cond_t go, done;
    int nthreads, active;
    unsigned gen;
    nitro_body body;
    size_t n;
    long next;
} nitro_pool = {.mu = PTHREAD_MUTEX_INITIALIZER, .submit = PTHREAD_MUTEX_INITIALIZER,
               .go = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER};
static __thread int nitro_in_loop;

static inline int nitro_ncpu(void);

static inline void nitro_run_items(void) {
    for (;;) {
        long i = __atomic_fetch_add(&nitro_pool.next, 1, __ATOMIC_RELAXED);
        if (i >= (long)nitro_pool.n) break;
        nitro_pool.body((size_t)i);
    }
}

static void *nitro_worker(void *arg) {
    (void)arg;
    nitro_in_loop = 1;
    unsigned seen = 0;
    pthread_mutex_lock(&nitro_pool.mu);
    for (;;) {
        while (nitro_pool.gen == seen) pthread_cond_wait(&nitro_pool.go, &nitro_pool.mu);
        seen = nitro_pool.gen;
        pthread_mutex_unlock(&nitro_pool.mu);
        nitro_run_items();
        pthread_mutex_lock(&nitro_pool.mu);
        if (--nitro_pool.active == 0) pthread_cond_signal(&nitro_pool.done);
    }
    return NULL;
}

// body(0) ... body(n-1) in parallel; returns when all are done.
static inline void nitro_parallel(size_t n, nitro_body body) {
    if (n <= 1 || nitro_in_loop) {
        for (size_t i = 0; i < n; i++) body(i);
        return;
    }
    pthread_mutex_lock(&nitro_pool.submit);
    if (!nitro_pool.nthreads) {   // the calling thread works too: ncpu - 1 workers
        int want = nitro_ncpu() - 1;
        for (int t = 0; t < want; t++) {
            pthread_t th;
            if (pthread_create(&th, NULL, nitro_worker, NULL)) break;
            pthread_detach(th);
            nitro_pool.nthreads++;
        }
        if (!nitro_pool.nthreads) nitro_pool.nthreads = -1;   // no threads: run sequentially
    }
    if (nitro_pool.nthreads < 0) {
        pthread_mutex_unlock(&nitro_pool.submit);
        for (size_t i = 0; i < n; i++) body(i);
        return;
    }
    pthread_mutex_lock(&nitro_pool.mu);
    nitro_pool.body = body;
    nitro_pool.n = n;
    nitro_pool.next = 0;
    nitro_pool.active = nitro_pool.nthreads;
    nitro_pool.gen++;
    pthread_cond_broadcast(&nitro_pool.go);
    pthread_mutex_unlock(&nitro_pool.mu);
    nitro_in_loop = 1;
    nitro_run_items();
    nitro_in_loop = 0;
    pthread_mutex_lock(&nitro_pool.mu);
    while (nitro_pool.active) pthread_cond_wait(&nitro_pool.done, &nitro_pool.mu);
    nitro_pool.body = NULL;
    pthread_mutex_unlock(&nitro_pool.mu);
    pthread_mutex_unlock(&nitro_pool.submit);
}

static inline double nitro_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
#endif

// ---------------------------------------------------------------------------
static inline int nitro_ncpu(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? (int)n : 8;
}

static inline void nitro_yield(void) { sched_yield(); }

// nitro_parallel, or the loop on the calling thread when nthreads is 1
static inline void nitro_parallel_n(size_t n, int nthreads, nitro_body body) {
    if (nthreads == 1)
        for (size_t i = 0; i < n; i++) body(i);
    else
        nitro_parallel(n, body);
}

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
