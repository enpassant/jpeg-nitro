// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroheic: see nitroheic.h
#include "nitroheic.h"
#ifdef NV_NITROHEVC
#include "nitrohevc.h"
#endif
#include "heif.h"   // the HEIF structure (shared with nitroavif)

#include "nitro_os.h"   // parallel loop (a thread pool)

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dispatch/dispatch.h>   // the VideoToolbox work: abandoned if it hangs (see decode_vt)
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int nh_read_info(const uint8_t *d, size_t len, nh_info *info) {
    Heif *h = calloc(1, sizeof *h);   // (big: not on the stack)
    if (!h) return -1;
    static const char *const brands[] = {"heic", "heix", "mif1", NULL};
    int rc = -1, wide_primaries = 0, hvcc_index = 0, npieces, gw, gh;
    size_t poff[8], plen[8];
    Box iref;
    uint32_t primary;
    if (heif_open(h, d, len, brands, &iref, &primary)) goto out;
    Item *pi = item(h, primary);
    memset(info, 0, sizeof *info);
    // the tiles: HEVC, one decoder configuration, one size, 8-bit 4:2:0, each in one piece;
    // alpha planes (auxl to the primary): not handled
    if (alpha_item(h, &iref, primary)) goto out;
    if (heif_tiles(h, &iref, pi, "hvc1", info->tile_off, info->tile_len, &info->ntiles, &info->rows, &info->cols,
                   &info->tile_w, &info->tile_h, &gw, &gh, poff, plen, &npieces, "hvcC", &hvcc_index) ||
        npieces)
        goto out;
    info->width = gw;
    info->height = gh;
    const Box *hc = &h->props[hvcc_index - 1];
    if (hc->end - hc->start < 23) goto out;
    const uint8_t *c = d + hc->start;
    if ((c[16] & 3) != 1 || (c[17] & 7) != 0 || (c[18] & 7) != 0) goto out;   // 4:2:0, 8-bit luma and chroma
    info->hvcc = c;
    info->hvcc_len = hc->end - hc->start;
    if (info->tile_w < 16 || info->tile_h < 16 || (info->tile_w | info->tile_h) & 1 || info->tile_w > 8192 ||
        info->tile_h > 8192)
        goto out;
    // clean aperture: only a crop from the top left corner (it then trims the right and bottom
    // edges, like the grid's output size); other crops go to ImageIO
    const Box *cl = prop(h, pi, "clap");
    if (cl) {
        double cw, ch, left, top;
        if (heif_clap(d, cl, info->width, info->height, &cw, &ch, &left, &top)) goto out;
        if (left != 0 || top != 0 || cw != (int)cw || ch != (int)ch || cw < 1 || ch < 1 || cw > info->width ||
            ch > info->height)
            goto out;
        info->width = (int)cw;
        info->height = (int)ch;
    }
    gw = info->cols * info->tile_w;
    gh = info->rows * info->tile_h;
    if (info->width < 1 || info->height < 1 || info->width > gw || info->height > gh) goto out;
    info->plane_w[0] = gw;
    info->plane_h[0] = gh;
    info->plane_w[1] = info->plane_w[2] = gw / 2;
    info->plane_h[1] = info->plane_h[2] = gh / 2;

    // rotation and mirroring (in the order of the properties), colour profile (of the primary image)
    Xf xf[8];
    info->orientation = heif_orientation(xf, heif_transforms(h, pi, xf, 8));
    int nclx, prim, matrix, full;
    heif_colr(h, pi, &info->icc, &info->icc_len, &nclx, &prim, &matrix, &full);
    if (nclx < 0) goto out;
    if (nclx) {   // the planes go to the GPU as full range BT.601 (as JPEG); colours: sRGB without a profile
        if (!full || (matrix != 2 && matrix != 5 && matrix != 6)) goto out;   // other YCbCr: ImageIO
        if (prim != 1 && prim != 2) wide_primaries = 1;   // (P3, BT.2020 ...: fine with an ICC profile)
    }
    if (wide_primaries && !info->icc) goto out;
    rc = 0;
out:
    free(h);
    return rc;
}

#ifdef NV_NITROHEVC
// ---- decoding with nitrohevc, our own decoder (built with make NITROHEVC=1; macOS: only with
// NITROHEVC=1 in the environment too)
static int decode_own(const uint8_t *data, const nh_info *info, uint8_t *const planes[3], const size_t pitch[3]) {
    _Atomic int failed = 0;
    _Atomic int *pfailed = &failed;
    nitro_parallel((size_t)info->ntiles, ^(size_t t) {
        int col = (int)t % info->cols, row = (int)t / info->cols;
        uint8_t *p[3];
        for (int c = 0; c < 3; c++) {
            int sh = c ? 1 : 0;
            p[c] = planes[c] + (size_t)(row * (info->tile_h >> sh)) * pitch[c] + (size_t)col * (info->tile_w >> sh);
        }
        // (a grid: the tiles in parallel, each on one thread; a single picture: its CTB rows in parallel)
        if (nhevc_decode(info->hvcc, info->hvcc_len, data + info->tile_off[t], info->tile_len[t], info->tile_w,
                         info->tile_h, p, pitch, info->ntiles > 1 ? 1 : 0))
            atomic_store(pfailed, 1);
    });
    return atomic_load(&failed) ? -1 : 0;
}
#endif

#ifdef __APPLE__
// ---- decoding: one VideoToolbox session per worker, tiles handed out in order
//
// VideoToolbox's HEVC decoders (software and hardware) can loop forever on a damaged stream: the
// decode call never returns, synchronous or not, and neither does invalidating the session (the
// decoder spins in its XPC service). So the decoding runs on a queue of its own and is waited for
// with a time limit; past it this fails (the caller falls back to ImageIO, which rejects such files
// quickly) and leaves the stuck work behind. That work owns all it uses (Run: a copy of the info and
// of the tile data) and writes into the caller's planes only while the caller still waits.

typedef struct {
    nh_info info;               // (its hvcc points into data)
    uint8_t *planes[3];
    size_t pitch[3];
    pthread_rwlock_t lock;      // tile_done reads `cancelled` (several at once); the caller sets it
    int cancelled;
    _Atomic int refs;           // the caller and the decoding work
    _Atomic int next, failed;
    dispatch_semaphore_t done;
    uint8_t *data;              // the tiles' data and the hvcC, copied
    size_t off[256];            // each tile's in data
} Run;

typedef struct {
    Run *run;
    int tile;
} Job;

static void run_release(Run *r) {
    if (atomic_fetch_sub(&r->refs, 1) != 1) return;
    pthread_rwlock_destroy(&r->lock);
    dispatch_release(r->done);
    free(r->data);
    free(r);
}

static void tile_done(void *ref, void *src, OSStatus st, VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts,
                      CMTime dur) {
    Job *j = src;
    Run *r = j->run;
    const nh_info *in = &r->info;
    if (st || !img || CVPixelBufferGetPixelFormatType(img) != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        (int)CVPixelBufferGetWidth(img) != in->tile_w || (int)CVPixelBufferGetHeight(img) != in->tile_h) {
        atomic_store(&r->failed, 1);
        return;
    }
    // BT.601 coefficients: matrix_coeffs 6 (ITU_R_601_4) or 5 (BT.470BG, as Samsung phones write it, which
    // VideoToolbox names "YCbCrMatrix#5"); BT.709 ...: ImageIO's job
    CFTypeRef matrix = CVBufferCopyAttachment(img, kCVImageBufferYCbCrMatrixKey, NULL);
    int other = matrix && !CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4) &&
                !CFEqual(matrix, CFSTR("YCbCrMatrix#5"));
    if (matrix) CFRelease(matrix);
    if (other) {
        atomic_store(&r->failed, 1);
        return;
    }
    pthread_rwlock_rdlock(&r->lock);
    if (!r->cancelled) {
        CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        int col = j->tile % in->cols, row = j->tile / in->cols;
        const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
        size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
        for (int k = 0; k < in->tile_h; k++)
            memcpy(r->planes[0] + (size_t)(row * in->tile_h + k) * r->pitch[0] + (size_t)col * in->tile_w, y + k * ys,
                   (size_t)in->tile_w);
        int cw = in->tile_w / 2, ch = in->tile_h / 2;
        for (int k = 0; k < ch; k++) {   // CbCr interleaved -> two planes
            const uint8_t *s = uv + k * uvs;
            uint8_t *cb = r->planes[1] + (size_t)(row * ch + k) * r->pitch[1] + (size_t)col * cw;
            uint8_t *cr = r->planes[2] + (size_t)(row * ch + k) * r->pitch[2] + (size_t)col * cw;
            for (int x = 0; x < cw; x++) {
                cb[x] = s[2 * x];
                cr[x] = s[2 * x + 1];
            }
        }
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    }
    pthread_rwlock_unlock(&r->lock);
}

// The decoding itself (on its own queue). Sets r->failed on any error.
static void vt_work(Run *r, int nthreads) {
    const nh_info *info = &r->info;
    CFDataRef hvcc = CFDataCreate(NULL, info->hvcc, (CFIndex)info->hvcc_len);
    const void *ak[] = {CFSTR("hvcC")}, *av[] = {hvcc};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef fmt = NULL;
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_HEVC, info->tile_w, info->tile_h, ext, &fmt);
    CFRelease(ext);
    CFRelease(atoms);
    CFRelease(hvcc);
    Job *jobs = calloc((size_t)info->ntiles, sizeof *jobs);
    if (st || !fmt || !jobs) {
        if (fmt) CFRelease(fmt);
        free(jobs);
        atomic_store(&r->failed, 1);
        return;
    }

    // Intel: the software decoder; in parallel it is several times faster than a hardware one
    // (a Radeon's serves one tile at a time). Apple Silicon: its media engine, with 4 sessions
    // (M1, 12 MP: ~26 ms; software ~65 ms, ImageIO ~59 ms)
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *pk[] = {kCVPixelBufferPixelFormatTypeKey}, *pv[] = {pfn};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, pk, pv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(pfn);
#if defined(__arm64__) || defined(__aarch64__)
    const CFBooleanRef hardware = kCFBooleanTrue;
    const int max_workers = 4;
#else
    const CFBooleanRef hardware = kCFBooleanFalse;
    const int max_workers = 8;
#endif
    const void *sk[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder}, *sv[] = {hardware};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    if (nthreads <= 0) {   // all CPUs, but at most max_workers: more decoders do not make it faster
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = n < 1 ? 1 : n > max_workers ? max_workers : (int)n;
    }
    if (nthreads > info->ntiles) nthreads = info->ntiles;
    dispatch_apply((size_t)nthreads, DISPATCH_APPLY_AUTO, ^(size_t w) {
        (void)w;
        VTDecompressionOutputCallbackRecord cb = {tile_done, NULL};
        VTDecompressionSessionRef s = NULL;
        if (VTDecompressionSessionCreate(NULL, fmt, spec, attrs, &cb, &s) || !s) {
            atomic_store(&r->failed, 1);
            return;
        }
        // asynchronous, all of this worker's tiles, then waited for once
        int t;
        while (!atomic_load(&r->failed) && (t = atomic_fetch_add(&r->next, 1)) < info->ntiles) {
            CMBlockBufferRef bb = NULL;
            CMSampleBufferRef sb = NULL;
            size_t n = info->tile_len[t];
            jobs[t] = (Job){r, t};
            if (CMBlockBufferCreateWithMemoryBlock(NULL, r->data + r->off[t], n, kCFAllocatorNull, NULL, 0, n, 0, &bb) ||
                CMSampleBufferCreateReady(NULL, bb, fmt, 1, 0, NULL, 1, &n, &sb) ||
                VTDecompressionSessionDecodeFrame(s, sb, kVTDecodeFrame_EnableAsynchronousDecompression, &jobs[t], NULL))
                atomic_store(&r->failed, 1);
            if (sb) CFRelease(sb);   // (VideoToolbox keeps what it still needs)
            if (bb) CFRelease(bb);
        }
        VTDecompressionSessionWaitForAsynchronousFrames(s);
        VTDecompressionSessionInvalidate(s);
        CFRelease(s);
    });
    free(jobs);
    CFRelease(spec);
    CFRelease(attrs);
    CFRelease(fmt);
}

static int decode_vt(const uint8_t *data, const nh_info *info, uint8_t *const planes[3], const size_t pitch[3],
                     int nthreads) {
    size_t total = info->hvcc_len;
    for (int t = 0; t < info->ntiles; t++) total += info->tile_len[t];
    Run *r = calloc(1, sizeof *r);
    if (!r || !(r->data = malloc(total))) {
        free(r);
        return -1;
    }
    r->info = *info;
    size_t at = 0;
    for (int t = 0; t < info->ntiles; t++) {
        r->off[t] = at;
        memcpy(r->data + at, data + info->tile_off[t], info->tile_len[t]);
        at += info->tile_len[t];
    }
    memcpy(r->data + at, info->hvcc, info->hvcc_len);
    r->info.hvcc = r->data + at;
    for (int c = 0; c < 3; c++) {
        r->planes[c] = planes[c];
        r->pitch[c] = pitch[c];
    }
    pthread_rwlock_init(&r->lock, NULL);
    r->done = dispatch_semaphore_create(0);
    atomic_store(&r->refs, 2);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        vt_work(r, nthreads);
        dispatch_semaphore_signal(r->done);
        run_release(r);
    });
    // the time limit: generous (12 MP: ~30-60 ms; a 25 MP single tile ~0.2 s; here 3.3 s)
    double mp = (double)info->plane_w[0] * info->plane_h[0] / 1e6;
    int64_t limit_ms = 2000 + (int64_t)(mp * 50);
    int rc;
    if (dispatch_semaphore_wait(r->done, dispatch_time(DISPATCH_TIME_NOW, limit_ms * (int64_t)NSEC_PER_MSEC))) {
        pthread_rwlock_wrlock(&r->lock);   // (waits for a tile being copied in)
        r->cancelled = 1;
        pthread_rwlock_unlock(&r->lock);
        rc = -1;
    } else {
        rc = atomic_load(&r->failed) ? -1 : 0;
    }
    run_release(r);
    return rc;
}
#endif

int nh_decoder(void) {
#if defined(__APPLE__) || defined(NV_NITROHEVC)
    return 1;
#else
    return 0;
#endif
}

int nh_decode_planes(const uint8_t *data, size_t len, const nh_info *info, uint8_t *const planes[3],
                     const size_t pitch[3], int nthreads) {
    (void)len;
    (void)nthreads;
#if defined(__APPLE__) && defined(NV_NITROHEVC)
    if (getenv("NITROHEVC")) return decode_own(data, info, planes, pitch);
#endif
#if defined(__APPLE__)
    return decode_vt(data, info, planes, pitch, nthreads);
#elif defined(NV_NITROHEVC)
    return decode_own(data, info, planes, pitch);
#else
    (void)data, (void)info, (void)planes, (void)pitch;
    return -1;   // no HEVC decoder in this build (see nh_decoder)
#endif
}
