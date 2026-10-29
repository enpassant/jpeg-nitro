// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_os.h: the little the decoders need from the OS - a parallel loop, a
// clock, the CPU count (and of the fast ones, on a CPU with performance and efficiency cores), yield. Header-only, included by every decoder (nitrojpeg,
// nitropng, nitropsd, nitroheic / nitrohevc, nitrodng, nitrowebp, nitrotiff,
// nitroavif) and the GNOME viewer's converter.
//
// The parallel loop is a pthread pool, started on first use, the same on every system (macOS,
// Linux, MinGW, other POSIX). macOS used Grand Central Dispatch until October 2026: the pool is as
// fast (on the M1 PNG 7% faster, HEIC through VideoToolbox 5% slower; the same on an Intel i9).
//
// The loop bodies are blocks (^(size_t i) { ... }): compile with clang, and
// outside macOS add -fblocks. The blocks are only called while nitro_parallel()
// runs (never copied), so no blocks runtime library is needed (see below).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __APPLE__
#include <sys/sysctl.h>   // nitro_ncpu_fast
#endif

#ifndef __BLOCKS__
#error "nitrojpeg / nitropng / nitropsd use blocks: compile with clang -fblocks"
#endif

typedef void (^nitro_body)(size_t);

// ---------------------------------------------------------------------------
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>   // nitro_pin_fast
#endif
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
// inside a loop body runs sequentially. A loop of no more items than performance
// cores (see nitro_ncpu_fast) runs on those: an efficiency core could only slow it down.
static struct {
    pthread_mutex_t mu, submit;
    pthread_cond_t go, done;
    int nthreads, active, fast;
    unsigned gen;
    nitro_body body;
    size_t n;
    long next;
} nitro_pool = {.mu = PTHREAD_MUTEX_INITIALIZER, .submit = PTHREAD_MUTEX_INITIALIZER,
               .go = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER};
static __thread int nitro_in_loop;

static inline int nitro_ncpu(void);
static inline int nitro_ncpu_fast(void);
static inline int nitro_pin_fast(int on);

static inline void nitro_run_items(void) {
    int pinned = 0;
    for (;;) {
        long i = __atomic_fetch_add(&nitro_pool.next, 1, __ATOMIC_RELAXED);
        if (i >= (long)nitro_pool.n) break;
        if (nitro_pool.fast && !pinned) pinned = nitro_pin_fast(1);   // (only threads that got an item)
        nitro_pool.body((size_t)i);
    }
    if (pinned) nitro_pin_fast(0);
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
    int fast = nitro_ncpu_fast();
    pthread_mutex_lock(&nitro_pool.mu);
    nitro_pool.body = body;
    nitro_pool.n = n;
    nitro_pool.fast = n <= (size_t)fast;
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

// The CPUs of the fastest kind on a CPU with performance and efficiency cores (Apple M1 and later,
// Intel 12th generation and later, ARM big.LITTLE), 0 when they are all alike. A loop whose
// workers wait for each other runs at the pace of its slowest core: such a loop uses only these
// (nitropng's filter reversal on the M1 under Linux: 4 threads 16% faster than 6).
static struct {
    int n;                    // nitro_ncpu_fast(), -1 until known
    unsigned long mask[16];   // Linux: which CPUs those are (CPU 0..1023)
} nitro_fast_cpus = {-1, {0}};

static inline int nitro_ncpu_fast(void) {
    int n = __atomic_load_n(&nitro_fast_cpus.n, __ATOMIC_ACQUIRE);
    if (n >= 0) return n;
    n = 0;
#if defined(__APPLE__)
    int levels = 0, fast = 0;   // perflevel0: the fastest
    size_t len = sizeof levels;
    if (!sysctlbyname("hw.nperflevels", &levels, &len, NULL, 0) && levels > 1) {
        len = sizeof fast;
        if (!sysctlbyname("hw.perflevel0.logicalcpu", &fast, &len, NULL, 0) && fast > 0) n = fast;
    }
#else
    int best = -1, nbest = 0, all = 0;
#if defined(_WIN32)   // the CPU sets' efficiency class: higher is faster (Windows 10: looked up,
    // the headers declare it only for _WIN32_WINNT >= 0x0A00)
    typedef BOOL(WINAPI * cpu_sets_fn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    cpu_sets_fn cpu_sets = (cpu_sets_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation");
    ULONG len = 0;
    if (cpu_sets) cpu_sets(NULL, 0, &len, GetCurrentProcess(), 0);
    char *buf = len ? malloc(len) : NULL;
    if (buf && cpu_sets((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        for (ULONG off = 0; off + sizeof(DWORD) * 2 <= len;) {
            const SYSTEM_CPU_SET_INFORMATION *s = (const SYSTEM_CPU_SET_INFORMATION *)(buf + off);
            if (!s->Size) break;
            if (s->Type == CpuSetInformation) {
                int c = s->CpuSet.EfficiencyClass;
                all++;
                if (c > best) best = c, nbest = 0;
                if (c == best) nbest++;
            }
            off += s->Size;
        }
    }
    free(buf);
#else   // Linux: the scheduler's relative capacity of each CPU (absent where they are all alike)
    long conf = sysconf(_SC_NPROCESSORS_CONF);
    int cap[1024];
    for (long cpu = 0; cpu < conf && cpu < 1024; cpu++) {
        char path[64];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpu_capacity", cpu);
        FILE *f = fopen(path, "r");
        cap[cpu] = -1;
        if (!f) continue;
        if (fscanf(f, "%d", &cap[cpu]) == 1) {
            all++;
            if (cap[cpu] > best) best = cap[cpu], nbest = 0;
            if (cap[cpu] == best) nbest++;
        }
        fclose(f);
    }
    for (long cpu = 0; nbest < all && cpu < conf && cpu < 1024; cpu++)
        if (cap[cpu] == best) nitro_fast_cpus.mask[cpu / 64] |= 1ul << (cpu % 64);
#endif
    if (nbest < all) n = nbest;
#endif
    __atomic_store_n(&nitro_fast_cpus.n, n, __ATOMIC_RELEASE);
    return n;
}

// on: the calling thread to the performance cores (those of them it may run on); 0 when it was not
// moved. off: back to the CPUs it had. Only Linux: macOS has no CPU affinity (its scheduler places the
// threads), on Windows it is not done (not measured: no CPU with efficiency cores to try it on).
static inline int nitro_pin_fast(int on) {
#if defined(__linux__)
    static __thread unsigned long saved[16];
    if (!on) return syscall(SYS_sched_setaffinity, 0, sizeof saved, saved) == 0;
    unsigned long want[16], any = 0;
    if (nitro_ncpu_fast() <= 0 || syscall(SYS_sched_getaffinity, 0, sizeof saved, saved) <= 0) return 0;
    for (int k = 0; k < 16; k++) any |= want[k] = saved[k] & nitro_fast_cpus.mask[k];
    return any && syscall(SYS_sched_setaffinity, 0, sizeof want, want) == 0;
#else
    (void)on;
    return 0;
#endif
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
