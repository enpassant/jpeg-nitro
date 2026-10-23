// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroview: minimal, very fast JPEG viewer for macOS.
//
//   nitroview [-f] [-s ms] [-j threads] file.jpg [more files or directories...]
//   nitroview --bench files...     decode everything, print timings, no window
//   nitroview --selftest files...  compare GPU colour conversion with libjpeg-turbo
//                                 (only in builds with the optional libjpeg-turbo fallback)
//
// Keys: PgDn/Space next, PgUp/Backspace previous, Home/End first/last,
//       +/- zoom, 0 fit, 1 actual size, arrows pan, F or Enter toggle
//       full screen, P pause slideshow,
//       Esc/Q quit.
//
// Pipeline: file -> nitrojpeg (multi-threaded, planar Y/Cb/Cr straight into a
// Metal buffer) -> GPU compute kernel (upsampling + YCbCr->RGB) -> mipmaps ->
// trilinear scaled draw. Neighbouring images are decoded ahead in the
// background, so paging is usually instantaneous.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <sys/stat.h>
#ifdef NV_TURBOJPEG
#include <turbojpeg.h>   // optional fallback for files nitrojpeg does not handle
#endif
#include <unistd.h>

#include "nitrojpeg.h"
#include "nitropng.h"
#include "nitropsd.h"
#include "nitroheic.h"
#include "nitrodng.h"
#include "nitrowebp.h"
#ifdef NV_WUFFS
#include "png_wuffs.h"   // optional fast PNG decoder
#endif
#ifdef NV_SVG
#include "nitrosvg.h"    // optional SVG rendering (lunasvg)
#endif
#ifdef NV_CDR
#include "nitrocdr.h"    // optional CorelDRAW rendering (libcdr + plutovg)
#endif
#if defined(NV_SVG) || defined(NV_CDR)
#define NV_VECTOR 1      // vector images: rendered again, sharp, when zoomed in
#include <stdatomic.h>
#endif
// renders a vector document's area into BGRA (see nitrosvg_render / nitrocdr_render)
typedef int (*VectorRender)(void *doc, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0);
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// Where decoded planes live. Shared: system memory, the GPU kernel reads it
// directly over PCIe (no driver copy). Managed: driver uploads to VRAM first.
static MTLResourceOptions g_storage = MTLResourceStorageModeShared;
static BOOL g_use_textures = NO;

#define MAX_TEX 16384
#define AHEAD 3     // images decoded ahead in the paging direction
#define BEHIND 2    // ... and kept / decoded behind

// ---------------------------------------------------------------------------
// decoded image

typedef enum { KIND_YUV = 0, KIND_BGRA = 1, KIND_PNG = 2, KIND_PLANAR = 3 } Kind;   // PNG: unfiltered rows; PLANAR: PSD planes

@interface Decoded : NSObject
@property(nonatomic) id<MTLBuffer> buffer;
@property(nonatomic) Kind kind;
@property(nonatomic) int width, height;      // decoded size
@property(nonatomic) int ncomp, orientation;
@property(nonatomic) id colorSpace;          // CGColorSpaceRef the pixel values are in (display tags the layer)
@property(nonatomic) int h0, v0, h1, v1, hmax, vmax;  // sampling (Y, chroma)
- (int *)pw;
- (int *)ph;
- (size_t *)offset;                         // plane offsets in the buffer
- (size_t *)pitch;
@property(nonatomic) double read_ms, decode_ms;
@property(nonatomic) int mode, bands;
@property(nonatomic) id<MTLTexture> texture; // converted RGBA + mips (main thread only)
@property(nonatomic) void *vector;           // SVG / CDR: the document, for sharp zoomed-in views
@property(nonatomic) VectorRender vectorRender;
@property(nonatomic) void (*vectorFree)(void *);
@property(nonatomic) double vectorScale;     // decoded pixels per document unit
@property(nonatomic) int pages, page;        // a multi-page document (CDR): page count, the page shown (from 0)
@property(nonatomic) int ev3;                // a DNG: the exposure correction it was developed with (1/3 EV)
@property(nonatomic) int limited;            // KIND_YUV: limited range (Y 16..235, as WebP), not full range
@end
@implementation Decoded {
    int _pw[3], _ph[3];
    size_t _offset[3], _pitch[3];
}
#ifdef NV_VECTOR
- (void)dealloc { if (_vector && _vectorFree) _vectorFree(_vector); }
#endif
- (int *)pw { return _pw; }
- (int *)ph { return _ph; }
- (size_t *)offset { return _offset; }
- (size_t *)pitch { return _pitch; }
@end

// Buffer pool: 50-100 MB Metal buffers are expensive to create (page faults),
// so evicted ones are reused.
@interface BufferPool : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)dev;
- (id<MTLBuffer>)get:(size_t)len;
- (void)put:(id<MTLBuffer>)b;
- (id<MTLTexture>)textureWidth:(int)w height:(int)h;   // RGBA8, mipmapped, private
- (void)putTexture:(id<MTLTexture>)t;
@end
@implementation BufferPool {
    id<MTLDevice> _dev;
    NSMutableArray<id<MTLBuffer>> *_free;
    NSMutableArray<id<MTLTexture>> *_freeTex;
}
- (instancetype)initWithDevice:(id<MTLDevice>)dev {
    if ((self = [super init])) { _dev = dev; _free = [NSMutableArray array]; _freeTex = [NSMutableArray array]; }
    return self;
}
- (id<MTLBuffer>)get:(size_t)len {
    @synchronized(self) {
        id<MTLBuffer> best = nil;
        for (id<MTLBuffer> b in _free)
            if (b.length >= len && b.length <= len * 2 && (!best || b.length < best.length)) best = b;
        if (best) { [_free removeObject:best]; return best; }
    }
    return [_dev newBufferWithLength:len options:g_storage];
}
- (void)put:(id<MTLBuffer>)b {
    if (!b) return;
    @synchronized(self) {
        [_free addObject:b];
        while (_free.count > 4) [_free removeObjectAtIndex:0];
    }
}
- (id<MTLTexture>)textureWidth:(int)w height:(int)h {
    @synchronized(self) {
        for (NSUInteger i = 0; i < _freeTex.count; i++) {
            id<MTLTexture> t = _freeTex[i];   // strong: removal must not free it
            if ((int)t.width == w && (int)t.height == h) { [_freeTex removeObjectAtIndex:i]; return t; }
        }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:YES];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    return [_dev newTextureWithDescriptor:td];
}
- (void)putTexture:(id<MTLTexture>)t {
    if (!t) return;
    @synchronized(self) {
        [_freeTex addObject:t];
        while (_freeTex.count > 4) [_freeTex removeObjectAtIndex:0];
    }
}
@end

static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static NSData *read_file(NSString *path) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) return nil;
    struct stat st;
    fstat(fd, &st);
    NSMutableData *d = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, (uint8_t *)d.mutableBytes + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == (size_t)st.st_size ? d : nil;
}

// Camera raw files (CR2, NEF ...): reads only the start (the TIFF directories) and the
// full-size JPEG nitrojpeg finds there, not the raw data (most of the 10-30 MB). nil if there
// is no such JPEG (the caller reads it all).
static NSData *read_raw_jpeg(NSString *path, int *orientation) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) return nil;
    NSData *out = nil;
    struct stat st;
    size_t cap = 1 << 20;   // (Nikon puts the SubIFD directories after the maker notes: ~270 KB in)
    uint8_t *hdr = malloc(cap);
    ssize_t n = hdr && !fstat(fd, &st) ? pread(fd, hdr, cap, 0) : -1;
    size_t off, len;
    if (n > 0 && !nj_find_embedded(hdr, (size_t)n, &off, &len, orientation) && off < (size_t)st.st_size &&
        len <= (size_t)st.st_size - off) {
        NSMutableData *d = [NSMutableData dataWithLength:len];
        size_t got = 0;
        while (got < len) {
            ssize_t r = pread(fd, (uint8_t *)d.mutableBytes + got, len - got, (off_t)(off + got));
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got == len) out = d;
    }
    free(hdr);
    close(fd);
    return out;
}

// PSD/PSB: reads only what the merged image needs. Big PSDs are mostly layer
// data (~80% of the file); the merged image is at the end. Builds a slim PSD in
// memory: header + colour mode data + image resources (ICC profile) + an empty
// layer section that keeps the layer count's sign (merged transparency) + the
// merged image data. Returns nil if the file is not a sane PSD (caller reads it all).
static NSData *read_psd_merged(NSString *path) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) return nil;
    NSMutableData *out = nil;
    struct stat st;
    uint8_t h[26], b[8];
    if (fstat(fd, &st) || pread(fd, h, 26, 0) != 26 || memcmp(h, "8BPS", 4)) goto done;
    {
        uint64_t size = (uint64_t)st.st_size;
        int psb = h[5] == 2;
        size_t L = psb ? 8 : 4;
        uint64_t pos = 26;
        for (int k = 0; k < 2; k++) {   // colour mode data, image resources
            if (pread(fd, b, 4, (off_t)pos) != 4) goto done;
            uint32_t l = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            pos += 4 + (uint64_t)l;
            if (pos > size) goto done;
        }
        uint64_t head = pos;   // bytes kept as they are
        if (pread(fd, b, L, (off_t)pos) != (ssize_t)L) goto done;
        uint64_t lm = 0;
        for (size_t i = 0; i < L; i++) lm = lm << 8 | b[i];
        uint64_t x = pos + L + lm;   // merged image data (compression field first)
        if (lm > size || x + 2 > size) goto done;
        int16_t count = 0;
        if (lm >= L + 2) {
            uint8_t c[10];
            if (pread(fd, c, L + 2, (off_t)(pos + L)) != (ssize_t)(L + 2)) goto done;
            uint64_t li = 0;
            for (size_t i = 0; i < L; i++) li = li << 8 | c[i];
            if (li >= 2) count = (int16_t)(c[L] << 8 | c[L + 1]);
        }
        uint64_t merged = size - x;
        out = [NSMutableData dataWithLength:(NSUInteger)(head + L + L + 2 + merged)];
        uint8_t *o = out.mutableBytes;
        if (pread(fd, o, (size_t)head, 0) != (ssize_t)head) { out = nil; goto done; }
        uint8_t *q = o + head;
        uint64_t slim = L + 2;   // layer and mask section: layer info (length 2) + layer count
        for (size_t i = 0; i < L; i++) q[i] = (uint8_t)(slim >> (8 * (L - 1 - i)));
        for (size_t i = 0; i < L; i++) q[L + i] = (uint8_t)(2 >> (8 * (L - 1 - i)));
        q[2 * L] = (uint8_t)((uint16_t)count >> 8);
        q[2 * L + 1] = (uint8_t)count;
        uint8_t *m = q + 2 * L + 2;
        size_t got = 0;
        while (got < merged) {
            ssize_t r = pread(fd, m + got, (size_t)(merged - got), (off_t)(x + got));
            if (r <= 0) { out = nil; goto done; }
            got += (size_t)r;
        }
    }
done:
    close(fd);
    return out;
}

// Decode one file into a Metal buffer. Runs on the decoder thread.
enum { MODE_TURBOJPEG = -1, MODE_WUFFS = -2, MODE_IMAGEIO = -3, MODE_NITROPNG = -4, MODE_NITROPSD = -5, MODE_SVG = -6, MODE_CDR = -7,
       MODE_NITROHEIC = -8, MODE_NITRODNG = -9, MODE_NITROWEBP = -10 };
static int g_nthreads = 0;   // -j
static _Atomic int g_dng_ev3;   // DNG exposure correction in 1/3 EV (keys E / Shift+E)

static const char *mode_name(int m) {
    switch (m) {
    case 2: return "split";
    case 1: return "RST";
    case 0: return "single";
    case MODE_TURBOJPEG: return "tj";
    case MODE_WUFFS: return "wuffs";
    case MODE_IMAGEIO: return "imageio";
    case MODE_NITROPNG: return "nitropng";
    case MODE_NITROPSD: return "nitropsd";
    case MODE_NITROHEIC: return "nitroheic";
    case MODE_NITRODNG: return "nitrodng";
    case MODE_NITROWEBP: return "nitrowebp";
    case MODE_SVG: return "svg";
    case MODE_CDR: return "cdr";
    default: return "?";
    }
}

// Fills d as a BGRA image in a fresh pool buffer.
static id<MTLBuffer> bgra_buffer(Decoded *d, int w, int h, BufferPool *pool, size_t align) {
    size_t pitch = align_up((size_t)w * 4, align);
    id<MTLBuffer> buf = [pool get:pitch * h];
    if (!buf) return nil;
    d.kind = KIND_BGRA;
    d.width = w;
    d.height = h;
    d.pitch[0] = pitch;
    d.bands = 1;
    return buf;
}

static void bgra_done(Decoded *d, id<MTLBuffer> buf, int mode, double t1) {
    if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, d.pitch[0] * d.height)];
    d.buffer = buf;
    d.mode = mode;
    d.decode_ms = now_ms() - t1;
}

// EXIF orientation of any image ImageIO understands (PNG eXIf, HEIC, TIFF ...).
static int imageio_orientation(CGImageSourceRef src) {
    NSDictionary *p = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, NULL));
    int o = [p[(id)kCGImagePropertyOrientation] intValue];
    return o >= 1 && o <= 8 ? o : 1;
}

static id srgb_space(void) {
    static id cs;
    if (!cs) cs = CFBridgingRelease(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    return cs;
}

// A JPEG's header segments up to and including SOS, without Extended XMP (some phones
// store megabytes in it: ImageIO parses it all, ~50 ms) and without the image data.
// nil if it is not a JPEG (or the headers are damaged): then ImageIO gets the whole file.
static NSData *jpeg_header_only(NSData *data) {
    static const char xmpext[] = "http://ns.adobe.com/xmp/extension/";
    const uint8_t *b = data.bytes;
    size_t n = data.length, p = 2;
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return nil;
    NSMutableData *out = [NSMutableData dataWithBytes:b length:2];
    while (p + 4 <= n && b[p] == 0xFF) {
        unsigned m = b[p + 1];
        if (m == 0xFF) { p++; continue; }   // fill byte
        size_t sl = 2 + ((size_t)b[p + 2] << 8 | b[p + 3]);
        if (sl < 4 || p + sl > n) return nil;
        int skip = m == 0xE1 && sl >= 4 + sizeof xmpext && !memcmp(b + p + 4, xmpext, sizeof xmpext);
        if (!skip) [out appendBytes:b + p length:sl];
        p += sl;
        if (m == 0xDA) {
            [out appendBytes:"\xFF\xD9" length:2];
            return out;
        }
    }
    return nil;
}

// The RGB colour space the file's pixel values are in: its embedded ICC profile
// (Display P3, Adobe RGB, a screen profile ...), or sRGB when it has none.
// Only the header is parsed: the CGImage is created lazily and never drawn.
static id image_colorspace(NSData *data) {
    id result = nil;
    NSData *hdr = jpeg_header_only(data);
    if (hdr) data = hdr;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)@{
                               (id)kCGImageSourceShouldCache: @NO}) : NULL;
    CGColorSpaceRef cs = img ? CGImageGetColorSpace(img) : NULL;
    if (cs && CGColorSpaceGetModel(cs) == kCGColorSpaceModelRGB) {
        NSString *name = CFBridgingRelease(CGColorSpaceCopyName(cs));
        if (!name || ![name isEqualToString:(__bridge NSString *)kCGColorSpaceGenericRGB]) result = (__bridge id)cs;
    }
    if (img) CGImageRelease(img);
    if (src) CFRelease(src);
    return result ?: srgb_space();
}

// Apple ImageIO: PNG (without Wuffs), HEIC, TIFF, WebP, GIF, BMP, PSD (composite),
// and JPEGs nothing else could decode. Huge images are downscaled to MAX_TEX.
static Decoded *decode_imageio(NSData *data, Decoded *d, BufferPool *pool, size_t align, double t1) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (!src || CGImageSourceGetCount(src) < 1) { if (src) CFRelease(src); return nil; }
    NSDictionary *p = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, NULL));
    long pw = [p[(id)kCGImagePropertyPixelWidth] longValue], ph = [p[(id)kCGImagePropertyPixelHeight] longValue];
    CGImageRef img;
    if (pw > MAX_TEX || ph > MAX_TEX) {
        NSDictionary *o = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                            (id)kCGImageSourceThumbnailMaxPixelSize: @(MAX_TEX),
                            (id)kCGImageSourceShouldCacheImmediately: @YES};
        img = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    } else {
        NSDictionary *o = @{(id)kCGImageSourceShouldCacheImmediately: @YES};
        img = CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    }
    d.orientation = imageio_orientation(src);
    CFRelease(src);
    if (!img) return nil;
    int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img);
    id<MTLBuffer> buf = w > 0 && h > 0 ? bgra_buffer(d, w, h, pool, align) : nil;
    if (!buf) { CGImageRelease(img); return nil; }
    // draw in the image's own colour space: values stay as in the file (no gamut clipping);
    // gray / indexed images are converted to sRGB
    if (!d.colorSpace) d.colorSpace = image_colorspace(data);
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, (size_t)w, (size_t)h, 8, d.pitch[0],
                                             (__bridge CGColorSpaceRef)d.colorSpace,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (!ctx) { CGImageRelease(img); [pool put:buf]; return nil; }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);   // premultiplied: transparent areas come out black
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    bgra_done(d, buf, MODE_IMAGEIO, t1);
    return d;
}

#ifdef NV_VECTOR
// Vector images (SVG, CorelDRAW): rendered once in full here (the long side 1024..4096
// pixels: small icons are enlarged, huge drawings reduced), over white. Zoomed in further,
// the viewer renders the visible part again at screen resolution (see ViewerView
// vectorDetail). Takes over the document (freed with the Decoded).
// Shown fitted to the screen (full screen or key 0), the viewer sets the screen's size in
// pixels here: images are then rendered at least that large, sharp from the start (the
// prefetched ones too), not first enlarged and sharpened a moment later by vectorDetail.
static _Atomic int g_vector_tw, g_vector_th;
static BOOL vector_fits(Decoded *d, int tw, int th) {   // big enough for the target (or no target)
    return tw <= 0 || th <= 0 || MIN((double)tw / d.width, (double)th / d.height) <= 1.01 ||
           MAX(d.width, d.height) >= MAX_TEX;
}
static Decoded *decode_vector(void *doc, double w, double h, VectorRender render, void (*vfree)(void *), int mode, Decoded *d,
                              BufferPool *pool, size_t align, double t1) {
    double l = MAX(w, h), k = l < 1024 ? 1024 / l : l > 4096 ? 4096 / l : 1;
    int tw = atomic_load(&g_vector_tw), th = atomic_load(&g_vector_th);
    if (tw > 0 && th > 0 && w > 0 && h > 0) k = MAX(k, MIN(tw / w, th / h));
    k = MIN(k, MAX_TEX / l);
    int iw = MAX(1, MIN(MAX_TEX, (int)lround(w * k))), ih = MAX(1, MIN(MAX_TEX, (int)lround(h * k)));
    id<MTLBuffer> buf = w > 0 && h > 0 ? bgra_buffer(d, iw, ih, pool, align) : nil;
    if (!buf || render(doc, buf.contents, iw, ih, d.pitch[0], k, 0, 0)) {
        if (buf) [pool put:buf];
        vfree(doc);
        return nil;
    }
    d.vector = doc;
    d.vectorRender = render;
    d.vectorFree = vfree;
    d.vectorScale = k;
    d.colorSpace = srgb_space();
    d.orientation = 1;
    bgra_done(d, buf, mode, t1);
    return d;
}
#endif

#ifdef NV_SVG
static int svg_render(void *doc, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    return nitrosvg_render(doc, bgra, w, h, pitch, scale, x0, y0);
}
static void svg_free(void *doc) { nitrosvg_free(doc); }
static Decoded *decode_svg(NSData *data, Decoded *d, BufferPool *pool, size_t align, double t1) {
    nitrosvg *svg = nitrosvg_load(data.bytes, data.length);
    if (!svg) return nil;
    double w, h;
    nitrosvg_size(svg, &w, &h);
    return decode_vector(svg, w, h, svg_render, svg_free, MODE_SVG, d, pool, align, t1);
}
#endif

#ifdef NV_CDR
// CorelDRAW (CDR, CMX): one page of the document; [ and ] show the others (cdr_page), sharing
// the loaded document (loading a large plan takes seconds, drawing a page much less)
typedef struct {
    nitrocdr *cdr;
    _Atomic int *refs;   // Decodeds sharing cdr
    int page;
} CdrPage;
static int cdr_render(void *doc, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    CdrPage *p = doc;
    return nitrocdr_render(p->cdr, p->page, bgra, w, h, pitch, scale, x0, y0);
}
static void cdr_free(void *doc) {
    CdrPage *p = doc;
    if (atomic_fetch_sub(p->refs, 1) == 1) {
        nitrocdr_free(p->cdr);
        free(p->refs);
    }
    free(p);
}
static Decoded *decode_cdr_page(nitrocdr *cdr, _Atomic int *refs, int page, Decoded *d, BufferPool *pool, size_t align,
                                double t1) {
    CdrPage *p = malloc(sizeof *p);
    if (!p) return nil;
    atomic_fetch_add(refs, 1);
    *p = (CdrPage){cdr, refs, page};
    double w, h;
    nitrocdr_size(cdr, page, &w, &h);
    d = decode_vector(p, w, h, cdr_render, cdr_free, MODE_CDR, d, pool, align, t1);   // (frees p on failure)
    d.pages = nitrocdr_pages(cdr);
    d.page = page;
    return d;
}
static Decoded *decode_cdr(NSData *data, Decoded *d, BufferPool *pool, size_t align, double t1) {
    nitrocdr *cdr = nitrocdr_load(data.bytes, data.length);
    _Atomic int *refs = cdr ? malloc(sizeof *refs) : NULL;
    if (!refs) {
        if (cdr) nitrocdr_free(cdr);
        return nil;
    }
    atomic_init(refs, 1);   // (this function's own reference, dropped below)
    d = decode_cdr_page(cdr, refs, 0, d, pool, align, t1);
    if (atomic_fetch_sub(refs, 1) == 1) {   // the first page failed
        nitrocdr_free(cdr);
        free(refs);
    }
    return d;
}
// Another page of the document d shows (a new Decoded, nil if it cannot be drawn).
static Decoded *cdr_page(Decoded *d, int page, BufferPool *pool, size_t align) {
    if (d.mode != MODE_CDR || !d.vector || page < 0 || page >= d.pages) return nil;
    CdrPage *p = d.vector;
    double t1 = now_ms();
    Decoded *n = decode_cdr_page(p->cdr, p->refs, page, [Decoded new], pool, align, t1);
    n.read_ms = d.read_ms;
    return n;
}
#endif

#ifdef NV_TURBOJPEG
static Decoded *decode_turbojpeg(const uint8_t *bytes, size_t len, Decoded *d, BufferPool *pool, size_t align, double t1) {
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    if (tj3DecompressHeader(h, bytes, len) < 0) { tj3Destroy(h); return nil; }
    int w = tj3Get(h, TJPARAM_JPEGWIDTH), ht = tj3Get(h, TJPARAM_JPEGHEIGHT);
    tjscalingfactor sf = {1, 1};
    while ((w + sf.denom - 1) / sf.denom > MAX_TEX || (ht + sf.denom - 1) / sf.denom > MAX_TEX) sf.denom *= 2;
    tj3SetScalingFactor(h, sf);
    w = TJSCALED(w, sf);
    ht = TJSCALED(ht, sf);
    id<MTLBuffer> buf = w > 0 && ht > 0 ? bgra_buffer(d, w, ht, pool, align) : nil;
    if (!buf) { tj3Destroy(h); return nil; }
    tj3Set(h, TJPARAM_STOPONWARNING, 0);
    int rc = tj3Decompress8(h, bytes, len, buf.contents, (int)d.pitch[0], TJPF_BGRX);
    int fatal = rc < 0 && tj3GetErrorCode(h) == TJERR_FATAL;
    tj3Destroy(h);
    if (fatal) { [pool put:buf]; return nil; }
    bgra_done(d, buf, MODE_TURBOJPEG, t1);
    return d;
}
#endif

static BOOL is_png(const uint8_t *b, size_t n) { return n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8); }

static Decoded *decode_file(NSString *path, BufferPool *pool, size_t align) {
    double t0 = now_ms();
    NSData *data = nil;
    NSString *ext = path.pathExtension.lowercaseString;
    if ([ext isEqualToString:@"psd"] || [ext isEqualToString:@"psb"]) {
        data = read_psd_merged(path);
        ps_info pi;   // only if nitropsd can decode it; ImageIO gets the whole file
        if (data && (ps_read_info(data.bytes, data.length, &pi) || !pi.supported || pi.width > MAX_TEX || pi.height > MAX_TEX))
            data = nil;
    }
    // a camera raw (CR2, NEF ...) shows its full-size JPEG (nitrojpeg finds it): only that is read
    int raw_orientation = 0, orient = 0;
    static NSSet *raw_exts;
    if (!raw_exts) raw_exts = [NSSet setWithArray:@[@"cr2", @"nef", @"nrw", @"arw", @"raf"]];
    int raw_read = 0;
    if (!data && [raw_exts containsObject:ext] && (data = read_raw_jpeg(path, &orient))) {
        raw_orientation = orient;   // (0: the JPEG's own, RAF)
        raw_read = 1;
    }
    if (!data) data = read_file(path);
    if (!data) return nil;
    ndng_info di;   // a DNG: its raw image developed by nitrodng (else its largest preview below)
    if (!raw_read && !ndng_read_info(data.bytes, data.length, &di) && di.width <= MAX_TEX && di.height <= MAX_TEX) {
        double t1 = now_ms();
        Decoded *d = [Decoded new];
        d.read_ms = t1 - t0;
        id<MTLBuffer> buf = bgra_buffer(d, di.width, di.height, pool, align);
        d.ev3 = atomic_load(&g_dng_ev3);
        di.ev = d.ev3 / 3.0f;
        if (buf && !ndng_decode(data.bytes, data.length, &di, buf.contents, d.pitch[0], g_nthreads)) {
            d.orientation = di.orientation;
            d.colorSpace = srgb_space();
            bgra_done(d, buf, MODE_NITRODNG, t1);
            return d;
        }
        if (buf) [pool put:buf];
    }
    size_t eoff, esize;
    if (!raw_read && !nj_find_embedded(data.bytes, data.length, &eoff, &esize, &orient) && eoff < data.length &&
        esize <= data.length - eoff) {   // (a raw file with another extension: read whole)
        data = [data subdataWithRange:NSMakeRange(eoff, esize)];
        raw_orientation = orient;
    }
    double t1 = now_ms();
    Decoded *d = [Decoded new];
    d.read_ms = t1 - t0;
    const uint8_t *bytes = data.bytes;
    size_t len = data.length;
#ifdef NV_SVG
    if ([ext isEqualToString:@"svg"]) return decode_svg(data, d, pool, align, t1);
#endif
#ifdef NV_CDR
    if ([ext isEqualToString:@"cdr"] || [ext isEqualToString:@"cmx"]) return decode_cdr(data, d, pool, align, t1);
#endif
    nh_info hi;   // HEIC (iPhone ...): its tiles decoded in parallel by nitroheic (VideoToolbox)
    if (!nh_read_info(bytes, len, &hi) && hi.plane_w[0] <= MAX_TEX && hi.plane_h[0] <= MAX_TEX) {
        size_t total = 0;
        for (int c = 0; c < 3; c++) {
            d.pitch[c] = align_up((size_t)hi.plane_w[c], align);
            d.offset[c] = total;
            total = align_up(total + d.pitch[c] * hi.plane_h[c], align);
            d.pw[c] = hi.plane_w[c];
            d.ph[c] = hi.plane_h[c];
        }
        id<MTLBuffer> buf = [pool get:total];
        if (buf) {
            uint8_t *base = buf.contents;
            uint8_t *planes[3] = {base + d.offset[0], base + d.offset[1], base + d.offset[2]};
            if (!nh_decode_planes(bytes, len, &hi, planes, d.pitch, g_nthreads)) {
                if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, total)];
                d.buffer = buf;
                d.kind = KIND_YUV;   // (full range BT.601 4:2:0, as a JPEG)
                d.width = hi.width;
                d.height = hi.height;
                d.ncomp = 3;
                d.h0 = 2; d.v0 = 2;
                d.h1 = 1; d.v1 = 1;
                d.hmax = 2; d.vmax = 2;
                d.orientation = hi.orientation;
                id cs = hi.icc ? CFBridgingRelease(CGColorSpaceCreateWithICCData(
                                     (__bridge CFDataRef)[NSData dataWithBytes:hi.icc length:hi.icc_len]))
                               : nil;
                d.colorSpace = cs ?: srgb_space();
                d.mode = MODE_NITROHEIC;
                d.bands = hi.ntiles;
                d.decode_ms = now_ms() - t1;
                return d;
            }
            [pool put:buf];
        }
    }
    nw_info wi;   // lossy WebP: nitrowebp (the tokens parsed on one thread, the rows reconstructed on another)
    if (!nw_read_info(bytes, len, &wi) && wi.plane_w[0] <= MAX_TEX && wi.plane_h[0] <= MAX_TEX) {
        size_t total = 0;
        for (int c = 0; c < 3; c++) {
            d.pitch[c] = align_up((size_t)wi.plane_w[c], align);
            d.offset[c] = total;
            total = align_up(total + d.pitch[c] * wi.plane_h[c], align);
            d.pw[c] = wi.plane_w[c];
            d.ph[c] = wi.plane_h[c];
        }
        id<MTLBuffer> buf = [pool get:total];
        if (buf) {
            uint8_t *base = buf.contents;
            uint8_t *planes[3] = {base + d.offset[0], base + d.offset[1], base + d.offset[2]};
            if (!nw_decode_planes(bytes, len, &wi, planes, d.pitch, g_nthreads)) {
                if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, total)];
                d.buffer = buf;
                d.kind = KIND_YUV;   // (limited range BT.601 4:2:0)
                d.limited = 1;
                d.width = wi.width;
                d.height = wi.height;
                d.ncomp = 3;
                d.h0 = 2; d.v0 = 2;
                d.h1 = 1; d.v1 = 1;
                d.hmax = 2; d.vmax = 2;
                d.orientation = wi.orientation;
                id cs = wi.icc ? CFBridgingRelease(CGColorSpaceCreateWithICCData(
                                     (__bridge CFDataRef)[NSData dataWithBytes:wi.icc length:wi.icc_len]))
                               : nil;
                d.colorSpace = cs ?: srgb_space();
                d.mode = MODE_NITROWEBP;
                d.bands = 1;
                d.decode_ms = now_ms() - t1;
                return d;
            }
            [pool put:buf];
        }
    }
    d.colorSpace = image_colorspace(data);   // tags the display layer (ICC profile or sRGB)
    if (is_png(bytes, len)) {
        // nitropng (parallel): 8-bit gray / gray+alpha / RGB / RGBA, non-interlaced
        np_info pi;
        if (!np_read_info(bytes, len, &pi) && pi.supported && pi.width <= MAX_TEX && pi.height <= MAX_TEX) {
            id<MTLBuffer> buf = [pool get:pi.raw_size];
            np_stats ps;
            if (buf && !np_decode(bytes, len, &pi, buf.contents, g_nthreads, &ps)) {
                CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
                d.orientation = src ? imageio_orientation(src) : 1;   // eXIf chunk
                if (src) CFRelease(src);
                d.kind = KIND_PNG;
                d.width = pi.width;
                d.height = pi.height;
                d.ncomp = pi.channels;
                d.pitch[0] = pi.stride + 1;   // rows keep their filter-type byte
                d.bands = ps.chunks;
                if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, pi.raw_size)];
                d.buffer = buf;
                d.mode = MODE_NITROPNG;
                d.decode_ms = now_ms() - t1;
                return d;
            }
            if (buf) [pool put:buf];
        }
#ifdef NV_WUFFS
        int w, h;
        if (!nv_png_size(bytes, len, &w, &h) && w <= MAX_TEX && h <= MAX_TEX) {
            id<MTLBuffer> buf = bgra_buffer(d, w, h, pool, align);
            if (buf && !nv_png_decode_bgra(bytes, len, buf.contents, d.pitch[0], w, h)) {
                CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
                d.orientation = src ? imageio_orientation(src) : 1;   // eXIf chunk (header only)
                if (src) CFRelease(src);
                bgra_done(d, buf, MODE_WUFFS, t1);
                return d;
            }
            if (buf) [pool put:buf];
        }
#endif
        return decode_imageio(data, d, pool, align, t1);
    }
    if (len >= 4 && !memcmp(bytes, "8BPS", 4)) {
        // nitropsd (parallel): merged image of 8-bit RGB / grayscale PSD and PSB
        ps_info pi;
        if (!ps_read_info(bytes, len, &pi) && pi.supported && pi.width <= MAX_TEX && pi.height <= MAX_TEX) {
            size_t P = (size_t)(pi.ncolor + pi.alpha);
            id<MTLBuffer> buf = [pool get:pi.plane_size * P];
            ps_stats ps;
            if (buf && !ps_decode(bytes, len, &pi, buf.contents, g_nthreads, &ps)) {
                d.kind = KIND_PLANAR;
                d.width = pi.width;
                d.height = pi.height;
                d.ncomp = pi.ncolor;
                d.h0 = pi.alpha;   // (reused field) 1: plane ncolor is the merged transparency
                d.pitch[0] = (size_t)pi.width;
                for (int c = 0; c < 3; c++) d.offset[c] = pi.ncolor == 3 ? c * pi.plane_size : 0;
                d.bands = 1;
                if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, pi.plane_size * P)];
                d.buffer = buf;
                d.mode = MODE_NITROPSD;
                d.decode_ms = now_ms() - t1;
                return d;
            }
            if (buf) [pool put:buf];
        }
        return decode_imageio(data, d, pool, align, t1);   // 16-bit, CMYK, Lab ... (no ZIP support there)
    }
    nj_info fi;
    int ok = !nj_read_info(bytes, len, &fi);
    d.orientation = raw_orientation ? raw_orientation : ok ? fi.orientation : 1;
    if (ok && fi.supported && fi.width <= MAX_TEX && fi.height <= MAX_TEX) {
        size_t total = 0;
        for (int c = 0; c < fi.ncomp; c++) {
            d.pitch[c] = align_up((size_t)fi.plane_w[c], align);
            d.offset[c] = total;
            total = align_up(total + d.pitch[c] * fi.plane_h[c], align);
            d.pw[c] = fi.plane_w[c];
            d.ph[c] = fi.plane_h[c];
        }
        id<MTLBuffer> buf = [pool get:total];
        if (!buf) return nil;
        uint8_t *base = buf.contents;
        uint8_t *planes[3] = {base + d.offset[0], base + d.offset[1], base + d.offset[2]};
        nj_stats st;
        if (!nj_decode_planes(bytes, len, &fi, planes, d.pitch, 0, &st)) {
            if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, total)];
            d.buffer = buf;
            d.kind = KIND_YUV;
            d.width = fi.width;
            d.height = fi.height;
            d.ncomp = fi.ncomp;
            d.h0 = fi.h[0]; d.v0 = fi.v[0];
            d.h1 = fi.ncomp > 1 ? fi.h[1] : 1; d.v1 = fi.ncomp > 1 ? fi.v[1] : 1;
            d.hmax = fi.hmax; d.vmax = fi.vmax;
            d.mode = st.mode;
            d.bands = st.bands;
            d.decode_ms = now_ms() - t1;
            return d;
        }
        [pool put:buf];
    }
#ifdef NV_TURBOJPEG
    {   // fallback: TurboJPEG (progressive, CMYK, RGB, huge images, damaged files)
        Decoded *t = decode_turbojpeg(bytes, len, d, pool, align, t1);
        if (t) return t;
    }
#endif
    return decode_imageio(data, d, pool, align, t1);   // anything else ImageIO can read
}

// ---------------------------------------------------------------------------
// GPU

#include "shaders.inc"   // src/shaders.metal as shaders_metal[] / shaders_metal_len (xxd -i)

typedef struct {
    uint32_t size[2];
    float cscale[2];
    float csize[2];
    uint32_t kind;
} ConvParams;

typedef struct {
    float origin[2];     // NDC position of the image's top-left corner
    float size[2];       // NDC extent
    uint32_t orientation;
    uint32_t nearest;    // 1: sample the nearest pixel (integer zoom >= 200%: pixel-exact blocks)
} DrawParams;

typedef struct {
    uint32_t size[2];
    float cscale[2];
    int32_t cmax[2];
    uint32_t off[3];
    uint32_t pitch[3];
    uint32_t kind;
    uint32_t aoff, hasa;   // PSD transparency plane
} PlaneParams;


@interface GPU : NSObject
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) id<MTLCommandQueue> queue;
@property(nonatomic, readonly) id<MTLComputePipelineState> convert, convertBuf;
@property(nonatomic, readonly) id<MTLRenderPipelineState> draw;
@property(nonatomic, readonly) size_t align;
- (instancetype)initWithDevice:(id<MTLDevice>)dev;
- (id<MTLTexture>)textureFor:(Decoded *)d commandBuffer:(id<MTLCommandBuffer>)cb;
@property(nonatomic) BufferPool *pool;
@end

@implementation GPU
- (instancetype)initWithDevice:(id<MTLDevice>)dev {
    if (!(self = [super init])) return nil;
    _device = dev;
    _queue = [dev newCommandQueue];
    NSError *err = nil;
    MTLCompileOptions *opt = [MTLCompileOptions new];   // fast math is the default
    id<MTLLibrary> lib = [dev newLibraryWithSource:[[NSString alloc] initWithBytes:shaders_metal length:shaders_metal_len encoding:NSUTF8StringEncoding]
                                                  options:opt error:&err];
    if (!lib) { NSLog(@"shader: %@", err); exit(1); }
    _convert = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"convert"] error:&err];
    _convertBuf = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"convert_buf"] error:&err];
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = [lib newFunctionWithName:@"vmain"];
    rp.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _draw = [dev newRenderPipelineStateWithDescriptor:rp error:&err];
    if (!_convert || !_draw) { NSLog(@"pipeline: %@", err); exit(1); }
    _align = MAX([dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR8Unorm],
                 [dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]);
    _align = MAX(_align, (size_t)64);
    return self;
}

- (id<MTLTexture>)planeTexture:(Decoded *)d index:(int)c format:(MTLPixelFormat)fmt w:(int)w h:(int)h {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeManaged;
    td.usage = MTLTextureUsageShaderRead;
    return [d.buffer newTextureWithDescriptor:td offset:d.offset[c] bytesPerRow:d.pitch[c]];
}

// Upload + convert + build mipmaps (all on the GPU, asynchronously).
- (id<MTLTexture>)textureFor:(Decoded *)d commandBuffer:(id<MTLCommandBuffer>)cb {
    if (d.texture) return d.texture;
    id<MTLTexture> out = [_pool textureWidth:d.width height:d.height];
    if (!g_use_textures || d.kind == KIND_PNG || d.kind == KIND_PLANAR) {
        PlaneParams pp = {{(uint32_t)d.width, (uint32_t)d.height}, {1, 1}, {0, 0}, {0}, {0}, 0, 0, 0};
        for (int c = 0; c < 3; c++) { pp.off[c] = (uint32_t)d.offset[c]; pp.pitch[c] = (uint32_t)d.pitch[c]; }
        if (d.kind == KIND_BGRA) pp.kind = 2;
        else if (d.kind == KIND_PLANAR) {
            pp.kind = 4;
            pp.hasa = (uint32_t)d.h0;
            pp.aoff = (uint32_t)((size_t)d.ncomp * d.width * d.height);
        }
        else if (d.kind == KIND_PNG) {
            pp.kind = 3; pp.off[0] = 1; pp.off[1] = (uint32_t)d.ncomp;
            if (getenv("NV_DEBUG_DIFF")) {
                const uint8_t *b = d.buffer.contents;
                fprintf(stderr, "   PNG params: off %u/%u pitch %u kind %u size %ux%u, buffer[0..8]: %d %d %d %d %d %d %d %d %d, len %lu\n",
                        pp.off[0], pp.off[1], pp.pitch[0], pp.kind, pp.size[0], pp.size[1], b[0], b[1], b[2], b[3], b[4],
                        b[5], b[6], b[7], b[8], (unsigned long)d.buffer.length);
            }
        }
        else if (d.ncomp == 1) pp.kind = 1;
        else {
            if (d.limited) pp.kind = 5;
            pp.cscale[0] = (float)d.h1 / d.hmax;
            pp.cscale[1] = (float)d.v1 / d.vmax;
            pp.cmax[0] = d.pw[1] - 1;
            pp.cmax[1] = d.ph[1] - 1;
        }
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:_convertBuf];
        [ce setBuffer:d.buffer offset:0 atIndex:1];
        [ce setTexture:out atIndex:3];
        [ce setBytes:&pp length:sizeof pp atIndex:0];
        [ce dispatchThreadgroups:MTLSizeMake((d.width + 15) / 16, (d.height + 15) / 16, 1)
            threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be generateMipmapsForTexture:out];
        [be endEncoding];
        d.texture = out;
        return out;
    }
    id<MTLTexture> t0, t1 = nil, t2 = nil;
    ConvParams p = {{(uint32_t)d.width, (uint32_t)d.height}, {1, 1}, {1, 1}, 0};
    if (d.kind == KIND_BGRA) {
        t0 = [self planeTexture:d index:0 format:MTLPixelFormatBGRA8Unorm w:d.width h:d.height];
        p.kind = 2;
    } else {
        t0 = [self planeTexture:d index:0 format:MTLPixelFormatR8Unorm w:d.pw[0] h:d.ph[0]];
        if (d.ncomp == 3) {
            t1 = [self planeTexture:d index:1 format:MTLPixelFormatR8Unorm w:d.pw[1] h:d.ph[1]];
            t2 = [self planeTexture:d index:2 format:MTLPixelFormatR8Unorm w:d.pw[2] h:d.ph[2]];
            p.cscale[0] = (float)d.h1 / d.hmax;
            p.cscale[1] = (float)d.v1 / d.vmax;
            p.csize[0] = d.pw[1];
            p.csize[1] = d.ph[1];
            if (d.limited) p.kind = 5;
        } else {
            p.kind = 1;
        }
    }
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:_convert];
    [ce setTexture:t0 atIndex:0];
    [ce setTexture:t1 ?: t0 atIndex:1];
    [ce setTexture:t2 ?: t0 atIndex:2];
    [ce setTexture:out atIndex:3];
    [ce setBytes:&p length:sizeof p atIndex:0];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    [ce dispatchThreadgroups:MTLSizeMake((d.width + 15) / 16, (d.height + 15) / 16, 1) threadsPerThreadgroup:tg];
    [ce endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be generateMipmapsForTexture:out];
    [be endEncoding];
    d.texture = out;
    return out;
}
@end

// ---------------------------------------------------------------------------
// image cache + background decoder

@interface Loader : NSObject
- (instancetype)initWithFiles:(NSArray<NSString *> *)files pool:(BufferPool *)pool align:(size_t)align;
- (Decoded *)get:(NSInteger)i;               // nil if not decoded yet
- (BOOL)failed:(NSInteger)i;                 // could not be decoded at all
- (void)focus:(NSInteger)cur direction:(int)dir;
// New file list; the decoded image of oldIndex (if any) is kept as newIndex.
- (void)replaceFiles:(NSArray<NSString *> *)files keep:(NSInteger)oldIndex as:(NSInteger)newIndex;
// Multi-page documents: draws another page of image i in the background; it replaces the
// decoded image and onDecoded reports it (nothing happens if i was paged away or evicted).
- (void)showPage:(int)page of:(NSInteger)i;
// The size vector images are rendered for changed (see g_vector_tw): prefetched ones too small
// for it are dropped and rendered again (the current one is sharpened by vectorDetail).
- (void)vectorTargetChanged;
// The DNG exposure correction changed (g_dng_ev3): prefetched DNGs are dropped and developed again,
// the current one is replaced when its new development is ready (it stays on screen until then).
- (void)dngExposureChanged;
@property(nonatomic, copy) void (^onDecoded)(NSInteger index);
@end

@implementation Loader {
    NSArray<NSString *> *_files;
    BufferPool *_pool;
    size_t _align;
    NSMutableDictionary<NSNumber *, Decoded *> *_cache;
    NSMutableSet<NSNumber *> *_failed;
    NSInteger _cur;
    int _dir;
    NSCondition *_cond;
    unsigned _gen;   // bumped when the file list changes: in-flight decodes are dropped
}
- (instancetype)initWithFiles:(NSArray<NSString *> *)files pool:(BufferPool *)pool align:(size_t)align {
    if (!(self = [super init])) return nil;
    _files = files;
    _pool = pool;
    _align = align;
    _cache = [NSMutableDictionary dictionary];
    _failed = [NSMutableSet set];
    _dir = 1;
    _cond = [NSCondition new];
    NSThread *th = [[NSThread alloc] initWithTarget:self selector:@selector(run) object:nil];
    th.qualityOfService = NSQualityOfServiceUserInteractive;
    [th start];
    return self;
}
- (BOOL)failed:(NSInteger)i {
    [_cond lock];
    BOOL f = [_failed containsObject:@(i)];
    [_cond unlock];
    return f;
}
- (Decoded *)get:(NSInteger)i {
    [_cond lock];
    Decoded *d = _cache[@(i)];
    [_cond unlock];
    return d;
}
- (void)recycle:(Decoded *)old {   // buffers back to the pool (textures live on the main thread)
    id<MTLBuffer> b = old.buffer;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self->_pool putTexture:old.texture];
        old.texture = nil;
        [self->_pool put:b];
    });
}
- (void)replaceFiles:(NSArray<NSString *> *)files keep:(NSInteger)oldIndex as:(NSInteger)newIndex {
    [_cond lock];
    Decoded *keep = oldIndex >= 0 ? _cache[@(oldIndex)] : nil;
    for (NSNumber *k in _cache.allKeys)
        if (_cache[k] != keep) [self recycle:_cache[k]];
    _cache = [NSMutableDictionary dictionary];
    if (keep) _cache[@(newIndex)] = keep;
    BOOL keepFailed = oldIndex >= 0 && [_failed containsObject:@(oldIndex)];
    _failed = [NSMutableSet set];
    if (keepFailed) [_failed addObject:@(newIndex)];
    _files = [files copy];
    _cur = newIndex;
    _gen++;
    [_cond signal];
    [_cond unlock];
}
- (void)showPage:(int)page of:(NSInteger)i {
#ifdef NV_CDR
    Decoded *d = [self get:i];
    if (!d || page == d.page || page < 0 || page >= d.pages) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        Decoded *n;
        @autoreleasepool { n = cdr_page(d, page, self->_pool, self->_align); }
        if (!n) return;
        [self->_cond lock];
        BOOL current = self->_cache[@(i)] == d;
        if (current) self->_cache[@(i)] = n;
        [self->_cond unlock];
        [self recycle:current ? d : n];
        void (^cb)(NSInteger) = self.onDecoded;
        if (current && cb) dispatch_async(dispatch_get_main_queue(), ^{ cb(i); });
    });
#endif
}
- (void)vectorTargetChanged {
#ifdef NV_VECTOR
    int tw = atomic_load(&g_vector_tw), th = atomic_load(&g_vector_th);
    [_cond lock];
    for (NSNumber *k in _cache.allKeys) {
        Decoded *d = _cache[k];
        if (d.vector && k.integerValue != _cur && !vector_fits(d, tw, th)) {
            [self recycle:d];
            [_cache removeObjectForKey:k];
        }
    }
    [_cond signal];
    [_cond unlock];
#endif
}
static BOOL stale_dng(Decoded *d) { return d.mode == MODE_NITRODNG && d.ev3 != atomic_load(&g_dng_ev3); }
- (void)dngExposureChanged {
    [_cond lock];
    NSInteger ci = _cur;
    Decoded *cur = _cache[@(ci)];
    for (NSNumber *k in _cache.allKeys) {
        Decoded *d = _cache[k];
        if (d != cur && stale_dng(d)) {
            [self recycle:d];
            [_cache removeObjectForKey:k];
        }
    }
    NSString *path = cur && stale_dng(cur) ? _files[ci] : nil;
    unsigned gen = _gen;
    [_cond signal];
    [_cond unlock];
    if (!path) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        Decoded *n;
        @autoreleasepool { n = decode_file(path, self->_pool, self->_align); }
        if (!n) return;
        [self->_cond lock];
        Decoded *old = self->_cache[@(ci)];
        BOOL use = gen == self->_gen && old && stale_dng(old) && !stale_dng(n);   // (a later key press wins)
        if (use) self->_cache[@(ci)] = n;
        [self->_cond unlock];
        [self recycle:use ? old : n];
        void (^cb)(NSInteger) = self.onDecoded;
        if (use && cb) dispatch_async(dispatch_get_main_queue(), ^{ cb(ci); });
    });
}
- (void)focus:(NSInteger)cur direction:(int)dir {
    [_cond lock];
    _cur = cur;
    _dir = dir;
    [_cond signal];
    [_cond unlock];
}
// Priority order: current, then AHEAD in the paging direction, then BEHIND.
- (NSArray<NSNumber *> *)wantedLocked {
    NSMutableArray *w = [NSMutableArray array];
    NSInteger n = (NSInteger)_files.count;
    void (^add)(NSInteger) = ^(NSInteger i) {   // no wrap-around past the first/last image
        if (i >= 0 && i < n && ![w containsObject:@(i)]) [w addObject:@(i)];
    };
    add(_cur);
    for (int k = 1; k <= MAX(AHEAD, BEHIND); k++) {
        if (k <= AHEAD) add(_cur + _dir * k);
        if (k <= BEHIND) add(_cur - _dir * k);
    }
    return w;
}
- (void)run {
    for (;;) {
        [_cond lock];
        NSNumber *next = nil;
        NSArray *wanted;
        for (;;) {
            wanted = [self wantedLocked];
            for (NSNumber *i in wanted)
                if (!_cache[i] && ![_failed containsObject:i]) { next = i; break; }
            if (next) break;
            [_cond wait];
        }
        // evict everything outside the window
        for (NSNumber *k in _cache.allKeys)
            if (![wanted containsObject:k]) {
                [self recycle:_cache[k]];
                [_cache removeObjectForKey:k];
            }
        NSString *path = _files[next.integerValue];
        unsigned gen = _gen;
        [_cond unlock];

        Decoded *d;
        @autoreleasepool { d = decode_file(path, _pool, _align); }

        [_cond lock];
        BOOL stale = gen != _gen || (d && stale_dng(d));   // the file list or the DNG exposure changed meanwhile
        if (stale) { if (d) [self recycle:d]; }
        else if (d) _cache[next] = d;
        else [_failed addObject:next];
        [_cond unlock];
        if (stale) continue;
        if (!d) fprintf(stderr, "cannot decode %s\n", path.fileSystemRepresentation);
        void (^cb)(NSInteger) = self.onDecoded;
        if (cb) dispatch_async(dispatch_get_main_queue(), ^{ cb(next.integerValue); });
    }
}
@end

// ---------------------------------------------------------------------------
// window

// Window content size for an image of w x h pixels (after EXIF rotation):
// smaller than the screen: exactly 100% (one image pixel per device pixel,
// rounded up so it is never scaled down) unless 'upscale'; larger (or with
// 'upscale'): the image's aspect ratio, as large as fits the visible screen.
static NSSize window_content_size(double w, double h, NSWindowStyleMask mask, NSScreen *scr, BOOL upscale) {
    if (!scr) scr = NSScreen.mainScreen;
    NSRect vis = scr.visibleFrame;
    double bs = scr.backingScaleFactor > 0 ? scr.backingScaleFactor : 1;
    double tb = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100) styleMask:mask].size.height - 100;
    double maxW = vis.size.width, maxH = vis.size.height - tb;
    double pw = w / bs, ph = h / bs;   // points at 100%
    double k = MIN(maxW / pw, maxH / ph);
    if (!upscale) k = MIN(k, 1.0);
    double cw = k == 1 ? ceil(pw) : floor(pw * k);
    double ch = k == 1 ? ceil(ph) : floor(ph * k);
    return NSMakeSize(MAX(cw, 320), MAX(ch, 200));
}

// Largest window content size that fits the visible screen area.
static NSSize max_content_size(NSWindowStyleMask mask, NSScreen *scr) {
    if (!scr) scr = NSScreen.mainScreen;
    NSRect vis = scr.visibleFrame;
    double tb = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100) styleMask:mask].size.height - 100;
    return NSMakeSize(vis.size.width, vis.size.height - tb);
}

static NSArray<NSString *> *collect_files(NSArray<NSString *> *args);
static NSArray<NSString *> *dropped_paths(id<NSDraggingInfo> info);
@protocol NVOpener   // the app delegate: opens files / folders from anywhere
- (void)openPaths:(NSArray<NSString *> *)paths;
@end

@interface ViewerView : MTKView <MTKViewDelegate>
@property(nonatomic) NSArray<NSString *> *files;
@property(nonatomic) GPU *gpu;
@property(nonatomic) Loader *loader;
@property(nonatomic) NSInteger index;
@property(nonatomic) BOOL lazyDir;   // opened with one file: list its folder on the first paging
- (void)startSlideshow:(double)ms;
- (void)page:(NSInteger)delta;
- (void)openFiles:(NSArray<NSString *> *)files lazyDir:(BOOL)lazy;
- (void)windowToImage;
// used by --zoomtest
- (void)testKey:(char)k image:(Decoded *)d view:(CGSize)ds;
- (DrawParams)drawParams:(Decoded *)d view:(CGSize)ds;
- (double)testScale:(Decoded *)d view:(CGSize)ds;
- (BOOL)testZoomed;
- (void)testWheel:(double)f anchor:(CGPoint)a image:(Decoded *)d view:(CGSize)ds;
- (void)testDragX:(double)dx y:(double)dy image:(Decoded *)d view:(CGSize)ds;
- (void)testDoubleClick:(CGPoint)a image:(Decoded *)d view:(CGSize)ds;
- (void)testPinch:(double)magnification image:(Decoded *)d view:(CGSize)ds;
@end

@implementation ViewerView {
    double _requestTime;      // when the current image was requested
    BOOL _reported;
    BOOL _wasReady;           // already decoded when requested
    BOOL _started;
    // slideshow
    double _slideMs;          // 0: off
    BOOL _paused;
    NSInteger _shownIndex;    // last image actually drawn
    double _shownTime;
    unsigned _slideGen;       // invalidates pending slideshow steps
    // zoom / pan: kept when paging, so a series can be compared at the same spot
    BOOL _zoomed;             // NO: fit to window
    BOOL _fitScreen;          // fit mode also enlarges small images to the screen (key 0)
    double _scale;            // image pixels -> drawable (device) pixels
    double _cx, _cy;          // image point (display orientation, pixels) at the view centre
    CGPoint _dragLast;        // last mouse position while dragging
    double _pageAccum;        // Ctrl + trackpad scrolling: accumulated distance
    // window sizing
    BOOL _userSized;          // the user resized the window: stop following the image size
    BOOL _selfResizing;       // our own setFrame in progress
    BOOL _fsTransition;       // entering / leaving full screen
#ifdef NV_VECTOR
    // a vector image (SVG, CDR) zoomed in beyond its decoded size: the visible part rendered at screen resolution
    id<MTLTexture> _detailTex;
    Decoded *_detailFor;
    double _detailScale, _detailRect[4];   // the scale it was rendered for, its image rect (x y w h)
    Decoded *_wantFor;                      // the last requested detail (pending or rendering)
    double _wantScale, _wantRect[4];
    unsigned _detailReq;
    dispatch_queue_t _vectorQueue;
#endif
}
- (BOOL)acceptsFirstResponder { return YES; }

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray *p = dropped_paths(info);
    dispatch_async(dispatch_get_main_queue(), ^{ [(id<NVOpener>)NSApp.delegate openPaths:p]; });
    return p.count > 0;
}

- (void)go:(NSInteger)i dir:(int)dir {
    NSInteger n = (NSInteger)_files.count;
    i = MAX(0, MIN(i, n - 1));              // stop at the first/last image, no rollover
    if (i == _index && _started) return;
    _started = YES;
    _index = i;
    _requestTime = now_ms();
    _reported = NO;
    Decoded *ready = [_loader get:i];
    _wasReady = ready != nil;
    [_loader focus:i direction:dir];
    if (ready) [self fitWindowTo:ready];
    [self updateTitle];
    self.needsDisplay = YES;
}

// One file was given: now that the user pages, list its folder (only now, so macOS
// asks for folder access only when it is really needed). The current image keeps
// its decoded data and becomes its place in the folder.
- (void)expandDirectory {
    if (!_lazyDir) return;
    _lazyDir = NO;
    NSString *path = _files[_index];
    NSString *dir = path.stringByDeletingLastPathComponent;
    if (!dir.length) dir = @".";
    NSArray<NSString *> *list = collect_files(@[dir]);
    NSString *name = path.lastPathComponent;
    NSUInteger k = [list indexOfObjectPassingTest:^BOOL(NSString *f, NSUInteger i, BOOL *stop) {
        return [f.lastPathComponent isEqualToString:name];
    }];
    if (k == NSNotFound || list.count < 2) return;   // unreadable folder / nothing else there
    NSMutableArray *files = [list mutableCopy];
    files[k] = path;   // keep the path exactly as given
    if (_shownIndex == _index) _shownIndex = (NSInteger)k;
    [_loader replaceFiles:files keep:_index as:(NSInteger)k];
    _files = files;
    _index = (NSInteger)k;
    [self updateTitle];
}

- (void)page:(NSInteger)delta {
    [self expandDirectory];
    [self go:_index + delta dir:delta >= 0 ? 1 : -1];
}
- (void)pageFirst { [self expandDirectory]; [self go:0 dir:1]; }
- (void)pageLast { [self expandDirectory]; [self go:(NSInteger)_files.count - 1 dir:-1]; }

// New files (e.g. opened from the Finder while running).
- (void)openFiles:(NSArray<NSString *> *)files lazyDir:(BOOL)lazy {
    if (!files.count) return;
    _lazyDir = lazy;
    _files = files;
    _started = NO;
    _index = 0;
    _shownIndex = -1;
    [_loader replaceFiles:files keep:-1 as:0];
    [self go:0 dir:1];
}

- (void)updateTitle {
    Decoded *d = [_loader get:_index];
    NSString *name = _files[_index].lastPathComponent;
    NSString *t = d ? [NSString stringWithFormat:@"%@  %.0f%%%@  (%ld/%lu)  %dx%d  decode %.1f ms", name,
                                                 100 * [self currentScale:d view:self.drawableSize],
                                                 _zoomed ? @"" : @" (fit)", _index + 1, _files.count, d.width,
                                                 d.height, d.decode_ms]
                    : [NSString stringWithFormat:@"%@  (%ld/%lu)  %@", name, _index + 1, _files.count,
                                                 [_loader failed:_index] ? @"CANNOT DECODE (unsupported or damaged)" : @"loading..."];
    if (d.pages > 1) t = [t stringByAppendingFormat:@"  [page %d/%d]", d.page + 1, d.pages];
    if (d.mode == MODE_NITRODNG && d.ev3) t = [t stringByAppendingFormat:@"  %+.1f EV", d.ev3 / 3.0];
    if (_slideMs > 0)
        t = [t stringByAppendingFormat:@"   %@", _paused ? @"[slideshow paused: P]"
                                                     : [NSString stringWithFormat:@"[slideshow %g ms]", _slideMs]];
    self.window.title = t;
}

// Slideshow: the next image comes exactly _slideMs after the current one was
// drawn. If decoding takes longer than that, it waits: no image is skipped.
- (void)startSlideshow:(double)ms {
    _slideMs = ms;
    _shownIndex = -1;
    [self updateTitle];
}

- (void)scheduleSlide {
    if (_slideMs > 0 && !_paused) [self expandDirectory];
    if (_slideMs <= 0 || _paused || _index >= (NSInteger)_files.count - 1) return;   // stops at the last image
    unsigned gen = ++_slideGen;
    NSInteger idx = _index;
    double wait = _slideMs - (now_ms() - _shownTime);
    __weak ViewerView *ws = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(MAX(wait, 0) * 1e6)), dispatch_get_main_queue(), ^{
        ViewerView *v = ws;
        if (v && gen == v->_slideGen && idx == v->_index && !v->_paused) [v page:1];
    });
}

// The window follows the image: in fit mode it gets the image's size (see
// window_content_size); zoomed in, it is as large as the image at the current
// zoom, up to the screen size. Keeps its centre and stays on screen. Not after
// a manual resize (until W) and not in full screen.
- (void)sizeWindowFor:(Decoded *)d {
    NSWindow *w = self.window;
    if (!w || !d || _userSized || _fsTransition || self.isFullScreen) return;
    double iw, ih;
    display_size(d, &iw, &ih);
    NSSize cs;
    if (_zoomed) {
        double bs = w.backingScaleFactor > 0 ? w.backingScaleFactor : 1;
        NSSize mx = max_content_size(w.styleMask, w.screen);
        cs = NSMakeSize(MAX(320, MIN(mx.width, floor(iw * _scale / bs))),
                        MAX(200, MIN(mx.height, floor(ih * _scale / bs))));
    } else {
        cs = window_content_size(iw, ih, w.styleMask, w.screen, _fitScreen);
    }
    NSRect cur = [w contentRectForFrameRect:w.frame];
    if (fabs(cur.size.width - cs.width) < 0.5 && fabs(cur.size.height - cs.height) < 0.5) return;
    NSRect nf = [w frameRectForContentRect:NSMakeRect(floor(NSMidX(cur) - cs.width / 2),
                                                      floor(NSMidY(cur) - cs.height / 2), cs.width, cs.height)];
    NSRect vis = (w.screen ?: NSScreen.mainScreen).visibleFrame;
    if (NSMaxX(nf) > NSMaxX(vis)) nf.origin.x = NSMaxX(vis) - nf.size.width;
    if (nf.origin.x < vis.origin.x) nf.origin.x = vis.origin.x;
    if (NSMaxY(nf) > NSMaxY(vis)) nf.origin.y = NSMaxY(vis) - nf.size.height;
    if (nf.origin.y < vis.origin.y) nf.origin.y = vis.origin.y;
    _selfResizing = YES;
    [w setFrame:nf display:YES animate:NO];
    _selfResizing = NO;
    if (getenv("NV_DEBUG_WINDOW"))
        fprintf(stderr, "  window -> %.0fx%.0f pt (%s %.1f%%)\n", cs.width, cs.height, _zoomed ? "zoom" : "fit",
                100 * [self currentScale:d view:self.drawableSize]);
}

// Paging: follow the new image only in fit mode (zoomed: keep window, zoom, position).
- (void)fitWindowTo:(Decoded *)d {
    if (!_zoomed) [self sizeWindowFor:d];
}

// Any resize we didn't do ourselves (dragging an edge, the green button,
// window tiling) counts as the user's choice: keep that size from now on.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc removeObserver:self];
    NSWindow *w = self.window;
    if (!w) return;
    [nc addObserver:self selector:@selector(windowResized:) name:NSWindowDidResizeNotification object:w];
    [nc addObserver:self selector:@selector(fsBegin:) name:NSWindowWillEnterFullScreenNotification object:w];
    [nc addObserver:self selector:@selector(fsBegin:) name:NSWindowWillExitFullScreenNotification object:w];
    [nc addObserver:self selector:@selector(fsEnd:) name:NSWindowDidEnterFullScreenNotification object:w];
    [nc addObserver:self selector:@selector(fsEnd:) name:NSWindowDidExitFullScreenNotification object:w];
}
- (void)windowResized:(NSNotification *)n {
    if (!_selfResizing && !_fsTransition && !self.isFullScreen) _userSized = YES;
}
- (void)fsBegin:(NSNotification *)n { _fsTransition = YES; }
- (void)fsEnd:(NSNotification *)n { _fsTransition = NO; }

// W: back to the image's own size (small images 100%), window follows it again.
// 0: fit to the screen, enlarging small images too; stays on while paging (until W).
- (void)fitToScreen {
    _fitScreen = YES;
    [self updateVectorTarget];
    [self zoomFit];
}

// Fitted to the screen (full screen or key 0): vector images are rendered for its size.
- (void)updateVectorTarget {
#ifdef NV_VECTOR
    NSScreen *sc = self.window.screen ?: NSScreen.mainScreen;
    BOOL fit = self.window && (self.isFullScreen || _fitScreen);
    int tw = fit ? (int)lround(sc.frame.size.width * sc.backingScaleFactor) : 0;
    int th = fit ? (int)lround(sc.frame.size.height * sc.backingScaleFactor) : 0;
    if (tw == atomic_load(&g_vector_tw) && th == atomic_load(&g_vector_th)) return;
    atomic_store(&g_vector_tw, tw);
    atomic_store(&g_vector_th, th);
    [_loader vectorTargetChanged];
#endif
}

- (void)windowToImage {
    _userSized = NO;
    _fitScreen = NO;
    [self updateVectorTarget];
    [self zoomFit];
    Decoded *d = [_loader get:_index];
    if (d) [self fitWindowTo:d];
}

- (void)imageDecoded:(NSInteger)i {
    Decoded *d = [_loader get:i];
    if (!d) {
        if (i == _index) {   // show the error, clear the old image; slideshow moves on
            [self updateTitle];
            self.needsDisplay = YES;
            if (_shownIndex != _index) { _shownIndex = _index; _shownTime = now_ms(); [self scheduleSlide]; }
        }
        return;
    }
    if (i == _index) {
        [self fitWindowTo:d];
        [self updateTitle];
        self.needsDisplay = YES;
    } else {
        // upload + convert neighbours ahead of time, so paging only has to draw
        NSInteger dist = labs(i - _index);
        if (dist <= 1 && !d.texture) {
            id<MTLCommandBuffer> cb = [_gpu.queue commandBuffer];
            [_gpu textureFor:d commandBuffer:cb];
            [cb commit];
        }
    }
}

// --- zoom / pan --- (view size is a parameter so --zoomtest can drive it offscreen)

static void display_size(Decoded *d, double *iw, double *ih) {   // after EXIF rotation
    BOOL swap = d.orientation >= 5;
    *iw = swap ? d.height : d.width;
    *ih = swap ? d.width : d.height;
}

- (BOOL)isFullScreen { return (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0; }

- (double)fitScale:(Decoded *)d view:(CGSize)ds {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = MIN(ds.width / iw, ds.height / ih);
    if (s > 1 && self.window && !self.isFullScreen && !_fitScreen) s = 1;   // small images: 100% (unless key 0)
    return s;
}
- (double)fitScale:(Decoded *)d { return [self fitScale:d view:self.drawableSize]; }
- (double)currentScale:(Decoded *)d view:(CGSize)ds { return _zoomed ? _scale : [self fitScale:d view:ds]; }

// Keeps the view centre inside the image (or centred where the image is smaller).
- (void)clampCenter:(Decoded *)d scale:(double)s view:(CGSize)ds {
    double iw, ih;
    display_size(d, &iw, &ih);
    double vw = ds.width / s, vh = ds.height / s;
    _cx = iw <= vw ? iw / 2 : MAX(vw / 2, MIN(_cx, iw - vw / 2));
    _cy = ih <= vh ? ih / 2 : MAX(vh / 2, MIN(_cy, ih - vh / 2));
}

- (void)zoomFit {
    _zoomed = NO;
    Decoded *d = self.window ? [_loader get:_index] : nil;
    if (d) [self sizeWindowFor:d];
    [self updateTitle];
    self.needsDisplay = YES;
}

// Sets the zoom so that the image point under 'a' (device pixels from the view
// centre, y down) stays under it. a = (0,0) zooms around the view centre.
- (void)setScale:(double)ns image:(Decoded *)d view:(CGSize)ds anchor:(CGPoint)a {
    double fit = [self fitSnapScale:d view:ds];
    if (fabs(ns - fit) < 1e-9 && fabs(ns - 1.0) > 1e-9) { [self zoomFit]; return; }
    double iw, ih;
    display_size(d, &iw, &ih);
    double cur = [self currentScale:d view:ds], cx = iw / 2, cy = ih / 2;
    if (_zoomed) {
        [self clampCenter:d scale:_scale view:ds];
        cx = _cx;
        cy = _cy;
    }
    double px = cx + a.x / cur, py = cy + a.y / cur;   // image point under the anchor
    _cx = px - a.x / ns;
    _cy = py - a.y / ns;
    _zoomed = YES;
    _scale = ns;
    [self clampCenter:d scale:ns view:ds];
    if (self.window && CGSizeEqualToSize(ds, self.drawableSize)) [self sizeWindowFor:d];   // not for offscreen tests
    [self updateTitle];
    self.needsDisplay = YES;
}

- (void)zoomTo:(double)ns image:(Decoded *)d view:(CGSize)ds {
    [self setScale:ns image:d view:ds anchor:CGPointZero];
}

// The "fit" zoom level used for snapping. While the window follows the image, the
// current window may have shrunk with a zoomed-out image, so its own fit would move
// down with it (and zooming out would jump back to fit): use the scale of the
// normal image-sized window instead.
- (double)fitSnapScale:(Decoded *)d view:(CGSize)ds {
    NSWindow *w = self.window;
    if (!w || _userSized || _fsTransition || self.isFullScreen || !CGSizeEqualToSize(ds, self.drawableSize))
        return [self fitScale:d view:ds];
    double iw, ih;
    display_size(d, &iw, &ih);
    NSSize cs = window_content_size(iw, ih, w.styleMask, w.screen, _fitScreen);
    double bs = w.backingScaleFactor > 0 ? w.backingScaleFactor : 1;
    double s = MIN(cs.width * bs / iw, cs.height * bs / ih);
    return _fitScreen ? s : MIN(s, 1.0);
}

static int debug_zoom(void) {   // NV_DEBUG_ZOOM=1: log wheel / pinch events and what they did
    static int on = -1;
    if (on < 0) on = getenv("NV_DEBUG_ZOOM") != NULL;
    return on;
}

// continuous: trackpad pinch / scroll, many small steps (no 2% snap, it would swallow them)
- (void)zoomBy:(double)f image:(Decoded *)d view:(CGSize)ds anchor:(CGPoint)a continuous:(BOOL)continuous {
    double fit = [self fitSnapScale:d view:ds];
    double cur = [self currentScale:d view:ds];
    double ns = cur * f;
    // stop exactly at 100% and at "fit" when a step crosses them
    for (int k = 0; k < 2; k++) {
        double snap = k == 0 ? 1.0 : fit;
        if ((cur < snap - 1e-9 && ns > snap + 1e-9) || (cur > snap + 1e-9 && ns < snap - 1e-9)) {
            ns = snap;
            break;
        }
    }
    ns = MAX(MIN(fit, 1.0) / 4, MIN(ns, 32.0));
    double r = round(ns);                       // e.g. sqrt(2)^2 -> exactly 200%
    if (r >= 1 && fabs(ns - r) < 1e-6 * r) ns = r;
    if (!continuous && fabs(ns - fit) < 0.02 * fit) ns = fit;   // a step landing within 2% of fit is fit
    [self setScale:ns image:d view:ds anchor:a];
    if (debug_zoom())
        fprintf(stderr, "    zoomBy f %.4f%s: cur %.4f fit %.4f -> ns %.4f => scale %.4f %s  view %.0fx%.0f\n", f,
                continuous ? " (cont)" : "", cur, fit, ns, [self currentScale:d view:self.drawableSize],
                _zoomed ? "zoomed" : "FIT", self.drawableSize.width, self.drawableSize.height);
}

- (void)zoomBy:(double)f image:(Decoded *)d view:(CGSize)ds anchor:(CGPoint)a {
    [self zoomBy:f image:d view:ds anchor:a continuous:NO];
}

- (void)zoomBy:(double)f image:(Decoded *)d view:(CGSize)ds {
    [self zoomBy:f image:d view:ds anchor:CGPointZero];
}

- (void)panX:(double)fx y:(double)fy image:(Decoded *)d view:(CGSize)ds {   // fractions of the view size
    [self panPixelsX:-fx * ds.width y:-fy * ds.height image:d view:ds];
}

// Moves the image by (dx, dy) device pixels (y down), e.g. following the mouse.
- (void)panPixelsX:(double)dx y:(double)dy image:(Decoded *)d view:(CGSize)ds {
    if (!_zoomed) return;
    [self clampCenter:d scale:_scale view:ds];
    _cx -= dx / _scale;
    _cy -= dy / _scale;
    [self clampCenter:d scale:_scale view:ds];
    self.needsDisplay = YES;
}

// Double click: fit -> 100% at the clicked point, otherwise back to fit.
- (void)toggleActualSize:(Decoded *)d view:(CGSize)ds anchor:(CGPoint)a {
    if (_zoomed) [self zoomFit];
    else [self setScale:1.0 image:d view:ds anchor:a];
}

// --- mouse ---

- (CGPoint)anchorForEvent:(NSEvent *)e {   // device pixels from the view centre, y down
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    double bs = self.drawableSize.width / MAX(b.size.width, 1);
    return CGPointMake((p.x - NSMidX(b)) * bs, (NSMidY(b) - p.y) * bs);
}

- (void)mouseDown:(NSEvent *)e {
    Decoded *d = [_loader get:_index];
    if (!d) return;
    if (e.clickCount == 2) {
        [self toggleActualSize:d view:self.drawableSize anchor:[self anchorForEvent:e]];
        return;
    }
    _dragLast = [self anchorForEvent:e];
    if (_zoomed) [[NSCursor closedHandCursor] set];
}

- (void)mouseDragged:(NSEvent *)e {
    Decoded *d = [_loader get:_index];
    CGPoint p = [self anchorForEvent:e];
    if (d) [self panPixelsX:p.x - _dragLast.x y:p.y - _dragLast.y image:d view:self.drawableSize];
    _dragLast = p;
}

- (void)mouseUp:(NSEvent *)e { [[NSCursor arrowCursor] set]; }

// Right click: next image (Shift: previous). Side buttons: back / forward.
- (void)rightMouseDown:(NSEvent *)e {
    if (e.modifierFlags & NSEventModifierFlagShift) [self page:-1];
    else [self page:1];
}
- (void)otherMouseDown:(NSEvent *)e {
    if (e.buttonNumber == 3) [self page:-1];
    else if (e.buttonNumber == 4) [self page:1];
}

- (void)scrollWheel:(NSEvent *)e {
    double dy = e.scrollingDeltaY;
    if (debug_zoom())
        fprintf(stderr, "scroll dy %.2f precise %d phase %lu momentum %lu ctrl %d\n", dy, e.hasPreciseScrollingDeltas,
                (unsigned long)e.phase, (unsigned long)e.momentumPhase, !!(e.modifierFlags & NSEventModifierFlagControl));
    if (e.isDirectionInvertedFromDevice) dy = -dy;   // physical direction: forward / up = zoom in
    if (e.modifierFlags & NSEventModifierFlagControl) {   // Ctrl + wheel: paging (back = next)
        if (!e.hasPreciseScrollingDeltas) {
            if (dy < 0) [self page:1];
            else if (dy > 0) [self page:-1];
            return;
        }
        _pageAccum += dy;                                 // trackpad: one image per 40 px
        if (e.phase == NSEventPhaseBegan) _pageAccum = dy;
        while (_pageAccum <= -40) { _pageAccum += 40; [self page:1]; }
        while (_pageAccum >= 40) { _pageAccum -= 40; [self page:-1]; }
        return;
    }
    Decoded *d = [_loader get:_index];
    if (!d || dy == 0) return;
    double f = e.hasPreciseScrollingDeltas ? exp(dy * 0.01)          // trackpad: continuous
                                           : (dy > 0 ? M_SQRT2 : M_SQRT1_2);   // wheel: one step per notch
    [self zoomBy:f image:d view:self.drawableSize anchor:[self anchorForEvent:e] continuous:e.hasPreciseScrollingDeltas];
}

- (void)magnifyWithEvent:(NSEvent *)e {   // trackpad pinch
    if (debug_zoom()) fprintf(stderr, "pinch mag %.4f phase %lu\n", e.magnification, (unsigned long)e.phase);
    Decoded *d = [_loader get:_index];
    if (d) [self zoomBy:1 + e.magnification image:d view:self.drawableSize anchor:[self anchorForEvent:e] continuous:YES];
}

// Where the image is: scale and the device pixel of its top-left corner (snapped).
- (double)placement:(Decoded *)d view:(CGSize)ds x0:(double *)x0 y0:(double *)y0 {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = [self currentScale:d view:ds];
    if (_zoomed) [self clampCenter:d scale:s view:ds];
    double cx = _zoomed ? _cx : iw / 2, cy = _zoomed ? _cy : ih / 2;
    *x0 = round(ds.width / 2 - cx * s);
    *y0 = round(ds.height / 2 - cy * s);
    return s;
}

// Where to draw the image: NDC corner + extent (top-left snapped to device pixels).
- (DrawParams)drawParams:(Decoded *)d view:(CGSize)ds {
    double iw, ih, x0, y0;
    display_size(d, &iw, &ih);
    double s = [self placement:d view:ds x0:&x0 y0:&y0];
    DrawParams p = {{(float)(x0 / ds.width * 2 - 1), (float)(1 - y0 / ds.height * 2)},
                    {(float)(2 * iw * s / ds.width), (float)(2 * ih * s / ds.height)},
                    (uint32_t)d.orientation, (uint32_t)(s >= 2 && s == floor(s) && !d.vector)};
    return p;
}

// Test hook: apply one key action ("+", "-", "0", "1", "L", "R", "U", "D") without events.
- (void)testKey:(char)k image:(Decoded *)d view:(CGSize)ds {
    switch (k) {
    case '+': [self zoomBy:M_SQRT2 image:d view:ds]; break;
    case '-': [self zoomBy:M_SQRT1_2 image:d view:ds]; break;
    case '0': [self fitToScreen]; break;
    case 'Z': [self zoomFit]; break;   // (test code only, not the keyboard)
    case '[': case ']': if (d.pages > 1) [_loader showPage:d.page + (k == ']' ? 1 : -1) of:_index]; break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
        [self zoomTo:k - '0' image:d view:ds];
        break;
    case 'L': [self panX:-0.125 y:0 image:d view:ds]; break;
    case 'R': [self panX:0.125 y:0 image:d view:ds]; break;
    case 'U': [self panX:0 y:-0.125 image:d view:ds]; break;
    case 'D': [self panX:0 y:0.125 image:d view:ds]; break;
    }
}
- (double)testScale:(Decoded *)d view:(CGSize)ds { return [self currentScale:d view:ds]; }
- (void)testWheel:(double)f anchor:(CGPoint)a image:(Decoded *)d view:(CGSize)ds { [self zoomBy:f image:d view:ds anchor:a]; }
- (void)testDragX:(double)dx y:(double)dy image:(Decoded *)d view:(CGSize)ds { [self panPixelsX:dx y:dy image:d view:ds]; }
- (void)testDoubleClick:(CGPoint)a image:(Decoded *)d view:(CGSize)ds { [self toggleActualSize:d view:ds anchor:a]; }
- (void)testPinch:(double)m image:(Decoded *)d view:(CGSize)ds {   // as magnifyWithEvent:
    [self zoomBy:1 + m image:d view:ds anchor:CGPointZero continuous:YES];
}
- (BOOL)testZoomed { return _zoomed; }

- (void)keyDown:(NSEvent *)e {
    // zoom keys by character, so they work on any keyboard layout and the keypad
    NSString *ch = e.charactersIgnoringModifiers;
    Decoded *cur = [_loader get:_index];
    if (ch.length == 1) {
        switch ([ch characterAtIndex:0]) {
        case '+': case '=': if (cur) [self zoomBy:M_SQRT2 image:cur view:self.drawableSize]; return;
        case '-': case '_': if (cur) [self zoomBy:M_SQRT1_2 image:cur view:self.drawableSize]; return;
        case '0': [self fitToScreen]; return;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':   // 100% .. 800%
            if (cur) [self zoomTo:[ch characterAtIndex:0] - '0' image:cur view:self.drawableSize];
            return;
        case 'w': case 'W': [self windowToImage]; return;
        case 'e': case 'E': {   // DNG: brighter / (Shift) darker, 1/3 EV
            if (!cur || cur.mode != MODE_NITRODNG) return;
            int ev3 = MAX(-9, MIN(9, atomic_load(&g_dng_ev3) + ((e.modifierFlags & NSEventModifierFlagShift) ? -1 : 1)));
            if (ev3 == atomic_load(&g_dng_ev3)) return;
            atomic_store(&g_dng_ev3, ev3);
            [_loader dngExposureChanged];
            return;
        }
        default: break;
        }
    }
    // [ ] (Option+F / G on a Hungarian keyboard, or the keys where US has them): the pages of a document
    unichar typed = e.characters.length == 1 ? [e.characters characterAtIndex:0] : 0;
    if (cur && cur.pages > 1 && (typed == '[' || typed == ']' || e.keyCode == 33 || e.keyCode == 30)) {
        _zoomed = NO;   // (pages may differ in size)
        [_loader showPage:cur.page + (typed == ']' || (typed != '[' && e.keyCode == 30) ? 1 : -1) of:_index];
        return;
    }
    double step = (e.modifierFlags & NSEventModifierFlagShift) ? 0.5 : 0.125;
    CGSize ds = self.drawableSize;
    switch (e.keyCode) {
    case 123: if (cur) [self panX:-step y:0 image:cur view:ds]; break;             // Left
    case 124: if (cur) [self panX:step y:0 image:cur view:ds]; break;              // Right
    case 126: if (cur) [self panX:0 y:-step image:cur view:ds]; break;             // Up
    case 125: if (cur) [self panX:0 y:step image:cur view:ds]; break;              // Down
    case 121: case 49: [self page:1]; break;                           // PgDn Space
    case 116: case 51: [self page:-1]; break;                          // PgUp Backspace
    case 115: [self pageFirst]; break;                                             // Home
    case 119: [self pageLast]; break;                                              // End
    case 3: case 36: [self.window toggleFullScreen:nil]; break;                    // F Return
    case 53:                                                                       // Esc
        if (self.window.styleMask & NSWindowStyleMaskFullScreen) [self.window toggleFullScreen:nil];
        else [NSApp terminate:nil];
        break;
    case 12: [NSApp terminate:nil]; break;                                         // Q
    case 35:                                                                       // P: pause slideshow
        if (_slideMs > 0) {
            _paused = !_paused;
            _slideGen++;
            _shownTime = now_ms();
            if (!_paused) [self scheduleSlide];
            [self updateTitle];
        }
        break;
    default: [super keyDown:e];
    }
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    [self updateVectorTarget];   // (full screen on / off, another screen)
    view.needsDisplay = YES;
    [self updateTitle];   // fit percentage changes with the window size
}

#ifdef NV_VECTOR
// A vector image (SVG, CDR) zoomed in beyond its decoded size (more than one device pixel per decoded pixel):
// once the zoom / position has been still for a moment, the visible part (+ a margin
// for panning) is rendered again at screen resolution on a background queue and
// drawn over the decoded image. Until then the previous detail is drawn where it
// belongs (scaled), so zooming stays smooth and only sharpens when it stops.
- (void)vectorDetail:(Decoded *)d encoder:(id<MTLRenderCommandEncoder>)re view:(CGSize)ds {
    if (_detailFor != d) { _detailTex = nil; _detailFor = nil; }
    double iw, ih, x0, y0;
    display_size(d, &iw, &ih);
    double s = [self placement:d view:ds x0:&x0 y0:&y0];
    if (s <= 1.001) {   // the decoded image is sharp enough
        _detailTex = nil;
        _detailFor = nil;
        _wantFor = nil;
        return;
    }
    // visible part in device pixels, integer, within the image
    double m = round(MAX(ds.width, ds.height) / 8);
    double l = MAX(floor(-m), x0), t = MAX(floor(-m), y0);
    double r = MIN(ceil(ds.width + m), x0 + ceil(iw * s)), b = MIN(ceil(ds.height + m), y0 + ceil(ih * s));
    double vl = MAX(0, x0), vt = MAX(0, y0), vr = MIN(ds.width, x0 + iw * s), vb = MIN(ds.height, y0 + ih * s);
    if (_detailTex) {
        double *q = _detailRect;
        DrawParams p = {{(float)((x0 + q[0] * s) / ds.width * 2 - 1), (float)(1 - (y0 + q[1] * s) / ds.height * 2)},
                        {(float)(2 * q[2] * s / ds.width), (float)(2 * q[3] * s / ds.height)}, 1, 0};
        [re setVertexBytes:&p length:sizeof p atIndex:0];
        [re setFragmentBytes:&p length:sizeof p atIndex:0];
        [re setFragmentTexture:_detailTex atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        // still right: same scale and it covers the view
        if (fabs(_detailScale - s) <= 1e-9 * s && x0 + q[0] * s <= vl + 0.5 && y0 + q[1] * s <= vt + 0.5 &&
            x0 + (q[0] + q[2]) * s >= vr - 0.5 && y0 + (q[1] + q[3]) * s >= vb - 0.5)
            return;
    }
    if (r <= l || b <= t) return;
    double want[4] = {(l - x0) / s, (t - y0) / s, (r - l) / s, (b - t) / s};
    if (_wantFor == d && _wantScale == s && !memcmp(want, _wantRect, sizeof want)) return;   // already on its way
    _wantFor = d;
    _wantScale = s;
    memcpy(_wantRect, want, sizeof want);
    unsigned req = ++_detailReq;
    CGRect wr = CGRectMake(want[0], want[1], want[2], want[3]);
    int w = MIN(MAX_TEX, (int)(r - l)), h = MIN(MAX_TEX, (int)(b - t));
    if (!_vectorQueue) _vectorQueue = dispatch_queue_create("nitroview.vector", DISPATCH_QUEUE_SERIAL);
    __weak ViewerView *ws = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        ViewerView *v = ws;
        if (!v || req != v->_detailReq) return;   // moved on meanwhile: a newer request follows
        double k = d.vectorScale;
        VectorRender render = d.vectorRender;
        dispatch_async(v->_vectorQueue, ^{
            size_t pitch = (size_t)w * 4;
            uint8_t *px = malloc(pitch * h);
            double t0 = now_ms();
            int rc = px ? render(d.vector, px, w, h, pitch, k * s, wr.origin.x / k, wr.origin.y / k) : -1;
            double ms = now_ms() - t0;
            dispatch_async(dispatch_get_main_queue(), ^{
                ViewerView *v2 = ws;
                if (v2 && !rc) [v2 vectorDetailDone:d pixels:px w:w h:h scale:s rect:wr ms:ms];
                free(px);
            });
        });
    });
}

- (void)vectorDetailDone:(Decoded *)d pixels:(const uint8_t *)px w:(int)w h:(int)h scale:(double)s
                 rect:(CGRect)rect ms:(double)ms {
    if (d != [_loader get:_index]) return;   // paged away
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w height:h mipmapped:YES];
    td.storageMode = MTLStorageModeManaged;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [_gpu.device newTextureWithDescriptor:td];
    if (!t) return;
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w * 4];
    id<MTLCommandBuffer> cb = [_gpu.queue commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be generateMipmapsForTexture:t];   // drawn smaller while zooming out, until the next one
    [be endEncoding];
    [cb commit];
    _detailTex = t;
    _detailFor = d;
    _detailScale = s;
    _detailRect[0] = rect.origin.x;
    _detailRect[1] = rect.origin.y;
    _detailRect[2] = rect.size.width;
    _detailRect[3] = rect.size.height;
    if (getenv("NV_DEBUG_SVG")) fprintf(stderr, "vector detail %dx%d at %.0f%%: %.1f ms\n", w, h, 100 * s, ms);
    self.needsDisplay = YES;
}
#endif

- (void)drawInMTKView:(MTKView *)view {
    MTLRenderPassDescriptor *rpd = view.currentRenderPassDescriptor;
    if (!rpd) return;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    id<MTLCommandBuffer> cb = [_gpu.queue commandBuffer];
    Decoded *d = [_loader get:_index];
    id<MTLTexture> tex = d ? [_gpu textureFor:d commandBuffer:cb] : nil;
    // not decoded yet (a slow raw / HEIC, a slideshow faster than decoding): the previous image stays
    // on screen until it is, rather than a black window (a failed one is not waited for)
    BOOL previous = NO;
    if (!tex && _shownIndex >= 0 && _shownIndex != _index && ![_loader failed:_index]) {
        Decoded *o = [_loader get:_shownIndex];
        if (o && (tex = [_gpu textureFor:o commandBuffer:cb])) {
            d = o;
            previous = YES;
        }
    }
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rpd];
    if (tex) {
        if (!previous && _shownIndex != _index) {
            _shownIndex = _index;
            _shownTime = now_ms();
            [self scheduleSlide];
        }
        DrawParams p = [self drawParams:d view:view.drawableSize];
        // colour management: the compositor converts from the image's space to the display
        CGColorSpaceRef ics = (__bridge CGColorSpaceRef)(d.colorSpace ?: srgb_space());
        if (!view.colorspace || !CFEqual(view.colorspace, ics)) view.colorspace = ics;
        [re setRenderPipelineState:_gpu.draw];
        [re setVertexBytes:&p length:sizeof p atIndex:0];
        [re setFragmentBytes:&p length:sizeof p atIndex:0];
        [re setFragmentTexture:tex atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
#ifdef NV_VECTOR
        if (d.vector && !previous) [self vectorDetail:d encoder:re view:view.drawableSize];
#endif
    }
    [re endEncoding];
    if (tex && !previous && !_reported) {
        _reported = YES;
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("NV_DEBUG_WINDOW") != NULL;
        if (dbg) {
            NSRect vis = NSScreen.mainScreen.visibleFrame;
            fprintf(stderr, "window content %.0fx%.0f pt, drawable %.0fx%.0f px, screen %.0fx%.0f pt @%gx, image %dx%d, %s %.1f%%\n",
                    self.bounds.size.width, self.bounds.size.height, view.drawableSize.width, view.drawableSize.height,
                    vis.size.width, vis.size.height, NSScreen.mainScreen.backingScaleFactor, d.width, d.height,
                    _zoomed ? "zoom" : "fit", 100 * [self currentScale:d view:view.drawableSize]);
            fprintf(stderr, "  title: %s\n", self.window.title.UTF8String);
        }
        double req = _requestTime;
        NSInteger idx = _index;
        NSString *name = _files[idx].lastPathComponent;
        BOOL wasCached = _wasReady;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            double total = now_ms() - req;
            printf("[%3ld] %-40s %5dx%-5d read %5.1f ms  decode %6.1f ms (%-7s %3d bands)  key->on screen %6.1f ms%s\n",
                   idx + 1, name.UTF8String, d.width, d.height, d.read_ms, d.decode_ms,
                   mode_name(d.mode), d.bands, total,
                   wasCached ? "  (prefetched)" : "");
            fflush(stdout);
        }];
    }
    [cb presentDrawable:view.currentDrawable];
    [cb commit];
}
@end

static ViewerView *create_viewer(NSArray<NSString *> *files, BOOL lazy, GPU *gpu);

// Accepts files / folders dragged onto a window (the viewer and the "open an image" window).
static NSArray<NSString *> *dropped_paths(id<NSDraggingInfo> info) {
    NSArray<NSURL *> *urls = [info.draggingPasteboard readObjectsForClasses:@[NSURL.class]
                                                                   options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) [paths addObject:u.path];
    return paths;
}
@interface DropView : NSView
@end
@implementation DropView
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray *p = dropped_paths(info);
    dispatch_async(dispatch_get_main_queue(), ^{ [(id<NVOpener>)NSApp.delegate openPaths:p]; });   // after the drag has ended
    return p.count > 0;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NVOpener>
@property(nonatomic) NSWindow *window;
@property(nonatomic) GPU *gpu;
@property(nonatomic) ViewerView *viewer;
@property(nonatomic) NSArray<NSString *> *argvFiles;   // opened from the command line already
@property(nonatomic) NSWindow *hint;                   // "open an image" window (app started empty)
@end
@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { return YES; }

// Files opened from the Finder, the Dock icon or `open -a nitroview.app file`.
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) if (u.isFileURL) [paths addObject:u.path];
    if (!paths.count) return;
    if (self.argvFiles && [paths isEqualToArray:self.argvFiles]) { self.argvFiles = nil; return; }   // AppKit echoes argv
    [self openPaths:paths];
}

// File > Open... (Cmd-O): images and / or folders.
- (void)openDocument:(id)sender {
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseFiles = YES;
    op.canChooseDirectories = YES;
    op.allowsMultipleSelection = YES;
    op.allowedContentTypes = @[UTTypeImage, UTTypeFolder];
    op.message = @"Images or folders to view";
    if ([op runModal] != NSModalResponseOK) return;
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in op.URLs) if (u.isFileURL) [paths addObject:u.path];
    [self openPaths:paths];
}

// Paths from the Finder, the Dock, a drop on a window or the Open panel: files and folders.
- (void)openPaths:(NSArray<NSString *> *)paths {
    if (!paths.count) return;
    NSApplication *app = NSApp;
    NSArray<NSString *> *files = collect_files(paths);
    if (!files.count) return;
    BOOL isDir = NO;
    BOOL lazy = paths.count == 1 && [NSFileManager.defaultManager fileExistsAtPath:paths[0] isDirectory:&isDir] && !isDir;
    if (self.viewer) {
        [self.viewer openFiles:files lazyDir:lazy];
        [self.viewer.window makeKeyAndOrderFront:nil];
    } else {
        [self.hint orderOut:nil];
        self.hint = nil;
        self.viewer = create_viewer(files, lazy, self.gpu);
        self.window = self.viewer.window;
    }
    [app activateIgnoringOtherApps:YES];
}
@end

// ---------------------------------------------------------------------------

static NSArray<NSString *> *collect_files(NSArray<NSString *> *args) {
    NSMutableArray *out = [NSMutableArray array];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *a in args) {
        BOOL dir = NO;
        if (![fm fileExistsAtPath:a isDirectory:&dir]) { fprintf(stderr, "no such file: %s\n", a.UTF8String); continue; }
        if (!dir) { [out addObject:a]; continue; }
        NSArray *items = [[fm contentsOfDirectoryAtPath:a error:nil] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
        for (NSString *f in items) {
            NSString *ext = f.pathExtension.lowercaseString;
            static NSSet *exts;
            if (!exts) exts = [NSSet setWithArray:@[@"jpg", @"jpeg", @"jpe", @"png", @"heic", @"heif", @"tif", @"tiff",
                                                    @"webp", @"gif", @"bmp", @"psd", @"psb", @"cr2", @"nef", @"nrw", @"arw", @"raf", @"dng",
#ifdef NV_SVG
                                                    @"svg",
#endif
#ifdef NV_CDR
                                                    @"cdr", @"cmx",
#endif
                                                    ]];
            if ([exts containsObject:ext])
                [out addObject:[a stringByAppendingPathComponent:f]];
        }
    }
    return out;
}

static int run_bench(NSArray<NSString *> *files, GPU *gpu) {
    BufferPool *pool = gpu.pool;
    double tr = 0, td = 0, tg = 0, t0 = now_ms();
    for (NSString *f in files) {
        @autoreleasepool {
            Decoded *d = decode_file(f, pool, gpu.align);
            if (!d) { printf("FAILED %s\n", f.UTF8String); continue; }
            double g0 = now_ms();
            id<MTLCommandBuffer> cb = [gpu.queue commandBuffer];
            [gpu textureFor:d commandBuffer:cb];
            [cb commit];
            [cb waitUntilCompleted];
            double g = now_ms() - g0;
            printf("%-45s %5dx%-5d read %5.1f  decode %6.1f  gpu %5.1f ms  (%s)\n", f.lastPathComponent.UTF8String, d.width,
                   d.height, d.read_ms, d.decode_ms, g, mode_name(d.mode));
            tr += d.read_ms; td += d.decode_ms; tg += g;
            [pool putTexture:d.texture];
            d.texture = nil;
            [pool put:d.buffer];
        }
    }
    double n = files.count;
    printf("\n%lu files: read %.1f ms (avg %.1f), decode %.1f ms (avg %.1f), gpu upload+convert %.1f ms (avg %.1f)\n"
           "total %.1f ms, avg %.1f ms/image\n",
           files.count, tr, tr / n, td, td / n, tg, tg / n, now_ms() - t0, (now_ms() - t0) / n);
    return 0;
}

#ifdef NV_TURBOJPEG
// Compare GPU conversion output with libjpeg-turbo's BGRX decode.
static int run_selftest(NSArray<NSString *> *files, GPU *gpu) {
    BufferPool *pool = gpu.pool;
    int bad = 0;
    for (NSString *f in files) {
        @autoreleasepool {
            Decoded *d = decode_file(f, pool, gpu.align);
            if (!d) { printf("FAILED %s\n", f.UTF8String); bad++; continue; }
            id<MTLCommandBuffer> cb = [gpu.queue commandBuffer];
            id<MTLTexture> tex = [gpu textureFor:d commandBuffer:cb];
            size_t rp = (size_t)d.width * 4;
            id<MTLBuffer> rb = [gpu.device newBufferWithLength:rp * d.height options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(d.width, d.height, 1) toBuffer:rb destinationOffset:0
            destinationBytesPerRow:rp destinationBytesPerImage:rp * d.height];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            NSData *data = read_file(f);
            uint8_t *ref = malloc(rp * d.height);
            const uint8_t *fb = data.bytes;
            int noref = 0;
            if (data.length > 2 && fb[0] == 0xFF && fb[1] == 0xD8) {   // JPEG: TurboJPEG as reference
                tjhandle h = tj3Init(TJINIT_DECOMPRESS);
                tj3Decompress8(h, data.bytes, data.length, ref, (int)rp, TJPF_RGBX);
                tj3Destroy(h);
            } else {   // PNG & co: Apple ImageIO as reference (premultiplied RGBA, like the viewer)
                memset(ref, 0, rp * d.height);
                CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
                CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
                CGColorSpaceRef cs = CGColorSpaceRetain((__bridge CGColorSpaceRef)d.colorSpace);   // no conversion
                CGContextRef ctx = CGBitmapContextCreate(ref, d.width, d.height, 8, rp, cs,
                                                         kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
                if (ctx && img) {
                    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
                    CGContextDrawImage(ctx, CGRectMake(0, 0, d.width, d.height), img);
                }
                if (!img) noref = 1;   // e.g. ZIP-compressed PSD: ImageIO can't read it
                if (ctx) CGContextRelease(ctx);
                CGColorSpaceRelease(cs);
                if (img) CGImageRelease(img);
                if (src) CFRelease(src);
            }
            const uint8_t *g = rb.contents;
            long hist[256] = {0};
            int maxd = 0, dbg_done = 0;
            if (noref) {
                printf("%-26.26s %-7s no ImageIO reference (ImageIO can't decode it)\n", f.lastPathComponent.UTF8String,
                       mode_name(d.mode));
                free(ref);
                [pool putTexture:d.texture];
                d.texture = nil;
                [pool put:d.buffer];
                continue;
            }
            double sum = 0;
            size_t cnt = 0;
            for (size_t k = 0; k < rp * d.height; k++) {
                if ((k & 3) == 3) continue;
                int e = abs((int)g[k] - ref[k]);
                hist[e]++;
                sum += e;
                cnt++;
                if (e > maxd) maxd = e;
                if (e > 8 && getenv("NV_DEBUG_DIFF") && !dbg_done) {
                    dbg_done = 1;
                    size_t px = (k % rp) / 4, py = k / rp;
                    const uint8_t *gg = g + py * rp + px * 4, *rr = ref + py * rp + px * 4;
                    printf("   first diff at (%zu,%zu): gpu %d %d %d %d  ref %d %d %d %d\n", px, py, gg[0], gg[1], gg[2], gg[3],
                           rr[0], rr[1], rr[2], rr[3]);
                }
            }
            NSString *csn = CFBridgingRelease(CGColorSpaceCopyName((__bridge CGColorSpaceRef)d.colorSpace));
            if (!csn) csn = @"(embedded ICC)";
            printf("%-26.26s %-7s %-22.22s max diff %3d  mean %.3f  <=1: %.4f%%\n", f.lastPathComponent.UTF8String,
                   mode_name(d.mode), [csn stringByReplacingOccurrencesOfString:@"kCGColorSpace" withString:@""].UTF8String, maxd, sum / cnt,
                   100.0 * (hist[0] + hist[1]) / cnt);
            if (maxd > 8) bad++;
            free(ref);
            d.texture = nil;
            [pool put:d.buffer];
        }
    }
    printf(bad ? "SELFTEST: %d problem(s)\n" : "SELFTEST OK\n", bad);
    return bad != 0;
}
#else
static int run_selftest(NSArray<NSString *> *files, GPU *gpu) {
    fprintf(stderr, "--selftest needs libjpeg-turbo as reference: build with TURBOJPEG=1\n");
    return 2;
}
#endif

// Offscreen check of zoom/pan: replays key sequences through the viewer's own
// code, renders into a texture and, at 100%, compares every drawn pixel with
// the decoded image.
static int run_zoomtest(NSArray<NSString *> *files, GPU *gpu) {
    const CGSize ds = {1600, 1000};
    const int W = (int)ds.width, H = (int)ds.height;
    ViewerView *v = [[ViewerView alloc] initWithFrame:NSMakeRect(0, 0, 800, 500) device:gpu.device];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [gpu.device newTextureWithDescriptor:td];
    id<MTLBuffer> out = [gpu.device newBufferWithLength:(size_t)W * H * 4 options:MTLResourceStorageModeShared];
    const char *scen[] = {"1", "1RRRDD", "1RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
                          "1UUUUUUUUUUUUUUUUUUUUUUUUUUUULLLLLLLLLLLLLLLLLLLLLLLLLLLLL", "1+-", "1-+", "+", "++--", "0",
                          "2", "2RRRDD", "1++", "1++++", "1+", "3", "5DDR", "8", "8LLU", NULL};
    int bad = 0;
    for (NSString *f in files) {
        @autoreleasepool {
            Decoded *d = decode_file(f, gpu.pool, gpu.align);
            if (!d) continue;
            id<MTLCommandBuffer> cb = [gpu.queue commandBuffer];
            id<MTLTexture> tex = [gpu textureFor:d commandBuffer:cb];
            size_t rp = (size_t)d.width * 4;
            id<MTLBuffer> ref = [gpu.device newBufferWithLength:rp * d.height options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(d.width, d.height, 1) toBuffer:ref destinationOffset:0
            destinationBytesPerRow:rp destinationBytesPerImage:rp * d.height];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            // scale sequence of repeated "+" from fit: must hit 100% exactly
            [v testKey:'0' image:d view:ds];
            NSMutableString *seq = [NSMutableString string];
            BOOL hit100 = [v testScale:d view:ds] >= 1.0;   // fits at 100% already: nothing to hit
            for (int k = 0; k < 16; k++) {
                [v testKey:'+' image:d view:ds];
                double sc = [v testScale:d view:ds];
                if (sc == 1.0) hit100 = YES;
                if (k < 8) [seq appendFormat:@"%.0f%% ", sc * 100];
            }
            printf("%-24s %5dx%-5d orient %d  '+' steps: %s... %s\n", f.lastPathComponent.UTF8String, d.width,
                   d.height, d.orientation, seq.UTF8String, hit100 ? "hits 100%" : "MISSES 100%");
            if (!hit100) bad++;
            for (int si = 0; scen[si]; si++) {
                [v testKey:'0' image:d view:ds];
                for (const char *k = scen[si]; *k; k++) [v testKey:*k image:d view:ds];
                DrawParams p = [v drawParams:d view:ds];
                double sc = [v testScale:d view:ds];
                id<MTLCommandBuffer> c2 = [gpu.queue commandBuffer];
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = target;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [c2 renderCommandEncoderWithDescriptor:rpd];
                [re setRenderPipelineState:gpu.draw];
                [re setVertexBytes:&p length:sizeof p atIndex:0];
                [re setFragmentBytes:&p length:sizeof p atIndex:0];
                [re setFragmentTexture:tex atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [re endEncoding];
                id<MTLBlitCommandEncoder> b2 = [c2 blitCommandEncoder];
                [b2 copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(W, H, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:(size_t)W * 4 destinationBytesPerImage:(size_t)W * H * 4];
                [b2 endEncoding];
                [c2 commit];
                [c2 waitUntilCompleted];
                // image rectangle in target pixels
                double x0 = (p.origin[0] + 1) / 2 * W, y0 = (1 - p.origin[1]) / 2 * H;
                double wpx = p.size[0] / 2 * W, hpx = p.size[1] / 2 * H;
                long ix0 = lround(x0), iy0 = lround(y0);
                const uint8_t *o = out.contents, *r = ref.contents;
                long checked = 0, diff = 0;
                if (sc >= 1 && sc == floor(sc) && d.orientation == 1) {   // integer zoom: exact pixel blocks
                    long isc = (long)sc;
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++) {
                            long dx = x - ix0, dy = y - iy0;
                            if (dx < 0 || dy < 0) continue;
                            long sx = dx / isc, sy = dy / isc;
                            if (sx >= d.width || sy >= d.height) continue;
                            const uint8_t *a = o + ((size_t)y * W + x) * 4, *b = r + (size_t)sy * rp + (size_t)sx * 4;
                            checked++;
                            if (a[2] != b[0] || a[1] != b[1] || a[0] != b[2]) diff++;   // BGRA vs RGBA
                        }
                }
                BOOL inside = x0 <= 0.5 || x0 + wpx >= W - 0.5 || fabs(x0 - (W - wpx) / 2) < 1;   // fills or centred
                BOOL insidey = y0 <= 0.5 || y0 + hpx >= H - 0.5 || fabs(y0 - (H - hpx) / 2) < 1;
                BOOL covers = (wpx < W || (x0 <= 0.5 && x0 + wpx >= W - 0.5)) && (hpx < H || (y0 <= 0.5 && y0 + hpx >= H - 0.5));
                BOOL ok = inside && insidey && covers && diff == 0;
                printf("   %-10.10s%s scale %6.1f%%  image at (%6.0f,%6.0f) %6.0fx%-6.0f %s%s\n", scen[si],
                       strlen(scen[si]) > 10 ? "…" : " ", sc * 100, x0, y0, wpx, hpx,
                       checked ? [NSString stringWithFormat:@"pixel-exact check %ld px, %ld differ  ", checked, diff].UTF8String : "",
                       ok ? "OK" : "FAIL");
                if (!ok) bad++;
            }
            // mouse: the image point under the cursor must stay under it when zooming,
            // and dragging must move the image by exactly the mouse movement
            {
                // image point under anchor a, from where the image is actually drawn
                CGPoint (^under)(CGPoint) = ^CGPoint(CGPoint a) {
                    DrawParams q = [v drawParams:d view:ds];
                    double sc = [v testScale:d view:ds];
                    double qx0 = (q.origin[0] + 1) / 2 * W, qy0 = (1 - q.origin[1]) / 2 * H;
                    return CGPointMake((W / 2.0 + a.x - qx0) / sc, (H / 2.0 + a.y - qy0) / sc);
                };
                // cursor inside the drawn image (fit mode): a quarter of the way to its corner
                [v testKey:'0' image:d view:ds];
                DrawParams fp = [v drawParams:d view:ds];
                CGPoint an = CGPointMake(fp.size[0] / 2 * W * 0.25, -fp.size[1] / 2 * H * 0.25);
                // along an axis where the image fits the view at 100% it stays centred, so the
                // point under the cursor can only be kept along the other axis
                double diw = d.orientation >= 5 ? d.height : d.width, dih = d.orientation >= 5 ? d.width : d.height;
                double axw = diw > W ? 1 : 0, ayw = dih > H ? 1 : 0;
                double (^drift)(CGPoint, CGPoint) = ^double(CGPoint a, CGPoint b) {
                    return hypot((a.x - b.x) * axw, (a.y - b.y) * ayw);
                };
                // wheel from 100% (the image covers the view, so the point can always stay put):
                // 4 notches in, 4 out, then 40 small trackpad-like steps in and out
                [v testKey:'1' image:d view:ds];
                CGPoint p0 = under(an);
                double worst = 0;
                for (int k = 0; k < 8; k++) {
                    [v testWheel:k < 4 ? M_SQRT2 : M_SQRT1_2 anchor:an image:d view:ds];
                    CGPoint p1 = under(an);
                    worst = MAX(worst, drift(p1, p0));   // image px = screen px at 100%
                }
                double scw = [v testScale:d view:ds];
                for (int k = 0; k < 80; k++) [v testWheel:k < 40 ? 1.02 : 1 / 1.02 anchor:an image:d view:ds];
                CGPoint p2 = under(an);
                double e2 = drift(p2, p0);
                BOOL wheelok = worst <= 1.0 && e2 <= 1.0 && scw == 1.0;
                printf("   wheel@(%+.0f,%+.0f) from 100%%: 4 notches in + 4 out -> %.0f%%, drift max %.2f px; 40+40 small steps -> drift %.2f px%s  %s\n",
                       an.x, an.y, scw * 100, worst, e2, axw && ayw ? "" : " (checked along the axis larger than the view)",
                       wheelok ? "OK" : "FAIL");
                if (!wheelok) bad++;
                // drag at 100% from the centre: image follows the mouse exactly
                [v testKey:'1' image:d view:ds];
                DrawParams a0 = [v drawParams:d view:ds];
                [v testDragX:137 y:-59 image:d view:ds];
                DrawParams a1 = [v drawParams:d view:ds];
                double mx = ((a1.origin[0] - a0.origin[0]) / 2) * W, my = -((a1.origin[1] - a0.origin[1]) / 2) * H;
                // the image only moves along axes where it is larger than the view (else it stays centred)
                BOOL big_x = d.width > W, big_y = d.height > H;
                BOOL dragok = fabs(mx - (big_x ? 137 : 0)) < 0.01 && fabs(my - (big_y ? -59 : 0)) < 0.01;
                printf("   drag (+137,-59) at 100%% -> image moved (%+.1f,%+.1f)%s  %s\n", mx, my,
                       big_x && big_y ? "" : " (image fits the view: stays centred)", dragok ? "OK" : "FAIL");
                if (!dragok) bad++;
                // double click in fit mode: 100% with the clicked point kept; again: back to fit
                [v testKey:'0' image:d view:ds];
                CGPoint c0 = under(an);
                [v testDoubleClick:an image:d view:ds];
                CGPoint c1 = under(an);
                double sc1 = [v testScale:d view:ds];
                double fs = fp.size[0] / 2 * W / (d.orientation >= 5 ? d.height : d.width);   // fit scale
                double ec = drift(c1, c0) * fs;   // drift in screen px at the clicked scale
                [v testDoubleClick:an image:d view:ds];
                BOOL dcok = sc1 == 1.0 && ec <= 1.0 && ![v testZoomed];
                printf("   double click -> %.0f%%, clicked point drift %.2f px, again -> %s  %s\n", sc1 * 100, ec,
                       [v testZoomed] ? "zoomed" : "fit", dcok ? "OK" : "FAIL");
                if (!dcok) bad++;
            }
            [gpu.pool putTexture:d.texture];
            d.texture = nil;
            [gpu.pool put:d.buffer];
        }
    }
    printf(bad ? "ZOOMTEST: %d problem(s)\n" : "ZOOMTEST OK\n", bad);
    return bad != 0;
}

// Image size after EXIF rotation, from the file header only (no decoding).
static BOOL header_size(NSString *path, double *w, double *h) {
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    if (!f) return NO;
    size_t cap = 1 << 20;   // headers (EXIF, previews, tables) practically always fit
    uint8_t *buf = malloc(cap);
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    ndng_info di;   // a DNG: its developed raw image
    if (!ndng_read_info(buf, n, &di)) {
        free(buf);
        BOOL swap = di.orientation >= 5;
        *w = swap ? di.height : di.width;
        *h = swap ? di.width : di.height;
        return YES;
    }
    nj_info fi;
    size_t eoff, esize;
    int raw_orientation = 0;   // camera raw (CR2, NEF ...): the header of its JPEG
    int raw = !nj_find_embedded(buf, n, &eoff, &esize, &raw_orientation);
    if (raw && (eoff >= n || n - eoff < 65536)) {   // the JPEG's header lies beyond what was read: read it
        FILE *g = fopen(path.fileSystemRepresentation, "rb");
        n = g && !fseeko(g, (off_t)eoff, SEEK_SET) ? fread(buf, 1, 65536, g) : 0;
        if (g) fclose(g);
        eoff = 0;
    }
    int ok = raw ? !nj_read_info(buf + eoff, n - eoff, &fi) && fi.width > 0 && fi.height > 0
                 : !nj_read_info(buf, n, &fi) && fi.width > 0 && fi.height > 0;
    if (raw && ok && raw_orientation) fi.orientation = raw_orientation;   // (RAF: the JPEG's own)
    free(buf);
    if (!ok && n == cap) {   // unusually large header: try the whole file
        NSData *all = read_file(path);
        ok = all && !nj_read_info(all.bytes, all.length, &fi) && fi.width > 0 && fi.height > 0;
    }
    int ow = fi.width, oh = fi.height, orient = fi.orientation;
    if (!ok) {   // PNG, HEIC, TIFF ...: ImageIO reads just the header
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
        if (!src) return NO;
        NSDictionary *p = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, NULL));
        CFRelease(src);
        ow = [p[(id)kCGImagePropertyPixelWidth] intValue];
        oh = [p[(id)kCGImagePropertyPixelHeight] intValue];
        orient = [p[(id)kCGImagePropertyOrientation] intValue];
        if (ow <= 0 || oh <= 0) return NO;
    }
    BOOL swap = orient >= 5;
    *w = swap ? oh : ow;
    *h = swap ? ow : oh;
    return YES;
}

// Initial window: sized to the first image (see window_content_size), centred
// on the visible screen area. Unknown size: 84% of the screen.
static NSRect initial_frame(NSString *first, NSWindowStyleMask mask) {
    NSScreen *scr = NSScreen.mainScreen;
    NSRect vis = scr.visibleFrame;
    double tb = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100) styleMask:mask].size.height - 100;
    double maxH = vis.size.height - tb, w, h;
    NSSize cs = header_size(first, &w, &h) ? window_content_size(w, h, mask, scr, NO)
                                           : NSMakeSize(floor(vis.size.width * 0.84), floor(maxH * 0.84));
    return NSMakeRect(vis.origin.x + floor((vis.size.width - cs.width) / 2),
                      vis.origin.y + floor((maxH - cs.height) / 2), cs.width, cs.height);
}

// Sends synthesized mouse / wheel events (as macOS would) to the viewer and
// checks the resulting image index and zoom.
static int run_inputtest(NSArray<NSString *> *files, GPU *gpu) {
    if (files.count < 5) { fprintf(stderr, "--inputtest needs at least 5 files\n"); return 2; }
    ViewerView *v = [[ViewerView alloc] initWithFrame:NSMakeRect(0, 0, 800, 500) device:gpu.device];
    v.files = files;
    v.gpu = gpu;
    v.loader = [[Loader alloc] initWithFiles:files pool:gpu.pool align:gpu.align];
    [v go:0 dir:1];
    for (int i = 0; i < 400 && ![v.loader get:0]; i++) usleep(5000);   // wait for the first image
    NSEvent * (^wheel)(double, CGEventFlags, BOOL) = ^NSEvent *(double dy, CGEventFlags fl, BOOL precise) {
        CGEventRef ce = CGEventCreateScrollWheelEvent(NULL, precise ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine,
                                                      1, (int32_t)dy);
        CGEventSetFlags(ce, fl);
        if (precise) CGEventSetIntegerValueField(ce, kCGScrollWheelEventIsContinuous, 1);
        NSEvent *e = [NSEvent eventWithCGEvent:ce];
        CFRelease(ce);
        return e;
    };
    NSEvent * (^mouse)(CGEventType, int, CGEventFlags) = ^NSEvent *(CGEventType t, int button, CGEventFlags fl) {
        CGEventRef ce = CGEventCreateMouseEvent(NULL, t, CGPointMake(100, 100), (CGMouseButton)button);
        CGEventSetIntegerValueField(ce, kCGMouseEventButtonNumber, button);
        CGEventSetFlags(ce, fl);
        NSEvent *e = [NSEvent eventWithCGEvent:ce];
        CFRelease(ce);
        return e;
    };
    struct { const char *what; NSEvent *ev; long expect; } steps[] = {
        {"Ctrl+wheel back",      wheel(-1, kCGEventFlagMaskControl, NO), 1},
        {"Ctrl+wheel back",      wheel(-1, kCGEventFlagMaskControl, NO), 2},
        {"Ctrl+wheel back",      wheel(-1, kCGEventFlagMaskControl, NO), 3},
        {"Ctrl+wheel forward",   wheel(+1, kCGEventFlagMaskControl, NO), 2},
        {"right click",          mouse(kCGEventRightMouseDown, 1, 0), 3},
        {"Shift+right click",    mouse(kCGEventRightMouseDown, 1, kCGEventFlagMaskShift), 2},
        {"side button forward",  mouse(kCGEventOtherMouseDown, 4, 0), 3},
        {"side button back",     mouse(kCGEventOtherMouseDown, 3, 0), 2},
        {"wheel without Ctrl",   wheel(+1, 0, NO), 2},                      // zooms, no paging
        {"Ctrl+trackpad 3x15px", NULL, 2}, {"(+15px)", NULL, 2}, {"(+15px) -> 45px", NULL, 1},
        {"Home: Ctrl+wheel fwd", NULL, 0},
        {"at first: forward",    wheel(+1, kCGEventFlagMaskControl, NO), 0},   // no rollover
    };
    int n = (int)(sizeof steps / sizeof *steps), bad = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(steps[i].what, "Ctrl+trackpad 3x15px") || !strcmp(steps[i].what, "(+15px)") ||
            !strcmp(steps[i].what, "(+15px) -> 45px"))
            [v scrollWheel:wheel(+15, kCGEventFlagMaskControl, YES)];
        else if (!strcmp(steps[i].what, "Home: Ctrl+wheel fwd"))
            for (int k = 0; k < 3; k++) [v scrollWheel:wheel(+1, kCGEventFlagMaskControl, NO)];
        else if (steps[i].ev.type == NSEventTypeScrollWheel) {
            if (i == 8)   // zooming needs the current image decoded
                for (int k = 0; k < 400 && ![v.loader get:v.index]; k++) usleep(5000);
            [v scrollWheel:steps[i].ev];
        }
        else if (steps[i].ev.type == NSEventTypeRightMouseDown) [v rightMouseDown:steps[i].ev];
        else [v otherMouseDown:steps[i].ev];
        BOOL ok = v.index == steps[i].expect;
        if (i == 8) ok = ok && [v testZoomed];   // plain wheel must have zoomed in
        printf("  %-22s -> image %ld (expected %ld)%s  %s\n", steps[i].what, (long)v.index + 1, steps[i].expect + 1,
               i == 8 ? ([v testZoomed] ? ", zoomed in" : ", NOT zoomed") : "", ok ? "OK" : "FAIL");
        if (!ok) bad++;
    }
    // trackpad: many tiny steps from fit (a pinch, a slow two-finger scroll) must zoom in,
    // not be snapped back to fit one by one
    Decoded *d = [v.loader get:v.index];
    for (int k = 0; k < 400 && !d; k++) { usleep(5000); d = [v.loader get:v.index]; }
    for (int t = 0; t < 2 && d; t++) {
        CGSize ds = v.drawableSize;
        [v testKey:'Z' image:d view:ds];
        double s0 = [v testScale:d view:ds];
        for (int k = 0; k < 20; k++) {
            if (t == 0) [v testPinch:0.01 image:d view:ds];
            else [v scrollWheel:wheel(+1, 0, YES)];   // 1 px trackpad scroll
        }
        double r = [v testScale:d view:ds] / s0;
        BOOL ok = [v testZoomed] && r > 1.15;
        printf("  %-22s -> zoom x%.3f  %s\n", t == 0 ? "pinch 20 x 1%" : "trackpad 20 x 1px", r, ok ? "OK" : "FAIL");
        if (!ok) bad++;
    }
    printf(bad ? "INPUTTEST: %d problem(s)\n" : "INPUTTEST OK\n", bad);
    return bad != 0;
}

// Viewer window for a list of files (lazy: one file given, its folder is listed on paging).
static ViewerView *create_viewer(NSArray<NSString *> *files, BOOL lazy, GPU *gpu) {
    NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                             NSWindowStyleMaskMiniaturizable;
    NSRect frame = initial_frame(files[0], mask);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:frame styleMask:mask backing:NSBackingStoreBuffered defer:NO];
    win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    win.backgroundColor = NSColor.blackColor;
    win.releasedWhenClosed = NO;
    ViewerView *v = [[ViewerView alloc] initWithFrame:frame device:gpu.device];
    v.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    v.colorspace = (__bridge CGColorSpaceRef)srgb_space();   // untagged content is sRGB
    v.paused = YES;
    v.enableSetNeedsDisplay = YES;
    v.delegate = v;
    v.files = files;
    v.gpu = gpu;
    v.loader = [[Loader alloc] initWithFiles:files pool:gpu.pool align:gpu.align];
    __weak ViewerView *wv = v;
    v.loader.onDecoded = ^(NSInteger i) { [wv imageDecoded:i]; };
    win.contentView = v;
    [v registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    [win makeFirstResponder:v];
    [win makeKeyAndOrderFront:nil];
    v.lazyDir = lazy;
    [v go:0 dir:1];
    return v;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        BOOL full = NO, bench = NO, selftest = NO, zoomtest = NO, inputtest = NO;
        double auto_ms = 0, slide_ms = 0;
        NSMutableArray *args = [NSMutableArray array];
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fullscreen")) full = YES;
            else if (!strcmp(argv[i], "--bench")) bench = YES;
            else if (!strcmp(argv[i], "--selftest")) selftest = YES;
            else if (!strcmp(argv[i], "--zoomtest")) zoomtest = YES;
            else if (!strcmp(argv[i], "--inputtest")) inputtest = YES;
            else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_ms = atof(argv[++i]);   // page every N ms, then quit
            else if (!strcmp(argv[i], "-j") && i + 1 < argc) {   // decoder threads
                g_nthreads = atoi(argv[++i]);
                nj_set_max_workers(g_nthreads);
            }
            else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--slideshow")) && i + 1 < argc) slide_ms = atof(argv[++i]);
            else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { args = nil; break; }
            else [args addObject:@(argv[i])];
        }
        NSArray *files = args ? collect_files(args) : @[];
        BOOL bundle = [NSBundle.mainBundle.bundlePath.pathExtension isEqualToString:@"app"];
        if (!files.count && !(bundle && args && !args.count)) {
            fprintf(stderr,
                "usage: nitroview [options] file.jpg|directory ...\n"
                "\n"
                "options:\n"
                "  -f, --fullscreen      start in full screen\n"
                "  -s, --slideshow MS    slideshow: next image after MS milliseconds on screen\n"
                "                        (waits for slow images, stops at the last one; P pauses)\n"
                "  -j N                  use N decoder threads (default: all logical CPUs)\n"
                "  --bench               load all files without a window, print timings\n"
                "  --selftest            compare GPU colour conversion with libjpeg-turbo%s\n"
                "  -h, --help            this help\n"
                "\n"
                "keys:\n"
                "  PgDn Space              next image         PgUp Backspace           previous\n"
                "  Home / End              first / last       F / Enter                full screen\n"
                "  + / -                   zoom in / out      0 fit to screen          1 actual size (1:1)\n"
                "  2 .. 8                  200%% .. 800%%, pixel-exact (1 image pixel = NxN screen pixels)\n"
                "  arrows                  move a zoomed image (Shift: bigger steps)\n"
                "  W                       back to the image's own size (small images 100%%, window follows)\n"
                "  E / Shift+E             DNG raw image: brighter / darker (1/3 EV steps, kept while paging)\n"
                "mouse:\n"
                "  drag                    move a zoomed image\n"
                "  scroll wheel            zoom around the cursor (trackpad: smooth, pinch too)\n"
                "  double click            fit <-> 100%% at the clicked point\n"
                "  right click             next image (Shift: previous)\n"
                "  Ctrl + scroll wheel     previous / next image\n"
                "  side buttons            previous / next image\n"
                "  P                       pause/resume slideshow\n"
                "  Esc                     leave full screen / quit                Q   quit\n",
#ifdef NV_TURBOJPEG
                ""
#else
                " (not in this build)"
#endif
            );
            return 1;
        }
        const char *stg = getenv("NV_STORAGE");
        if (stg && !strcmp(stg, "managed")) g_storage = MTLResourceStorageModeManaged;
        if (getenv("NV_TEXTURES")) g_use_textures = YES, g_storage = MTLResourceStorageModeManaged;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        GPU *gpu = [[GPU alloc] initWithDevice:dev];
        gpu.pool = [[BufferPool alloc] initWithDevice:dev];
        if (bench) return run_bench(files, gpu);
        if (selftest) return run_selftest(files, gpu);
        if (zoomtest) return run_zoomtest(files, gpu);
        if (inputtest) return run_inputtest(files, gpu);

        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *del = [AppDelegate new];
        app.delegate = del;

        NSMenu *bar = [NSMenu new], *appMenu = [NSMenu new];
        NSMenuItem *item = [NSMenuItem new];
        [bar addItem:item];
        NSMenuItem *open = [appMenu addItemWithTitle:@"Open..." action:@selector(openDocument:) keyEquivalent:@"o"];
        open.target = del;
        [appMenu addItem:NSMenuItem.separatorItem];
        [appMenu addItemWithTitle:@"Quit nitroview" action:@selector(terminate:) keyEquivalent:@"q"];
        item.submenu = appMenu;
        app.mainMenu = bar;

        del.gpu = gpu;
        if (!files.count) {   // app started from the Finder / Dock without a file: wait for "open" events
            NSWindow *hw = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 120)
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                         backing:NSBackingStoreBuffered defer:NO];
            hw.title = @"nitroview";
            DropView *dv = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, 420, 120)];
            [dv registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
            hw.contentView = dv;
            NSTextField *t = [NSTextField labelWithString:@"Drop images or a folder here or on the Dock icon,\n"
                                                           "choose Open... (\u2318O), or open images from the Finder."];
            t.frame = NSMakeRect(20, 30, 380, 60);
            [dv addSubview:t];
            [hw center];
            del.hint = hw;
            // shown only if no file arrives right away (a Finder "open" follows the launch)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                if (!del.viewer) [del.hint makeKeyAndOrderFront:nil];
            });
            [app activateIgnoringOtherApps:YES];
            [app run];
            return 0;
        }
        del.argvFiles = args;
        BOOL isDir = NO;
        ViewerView *v = create_viewer(files, args.count == 1 && [NSFileManager.defaultManager fileExistsAtPath:args[0]
                                                                                             isDirectory:&isDir] && !isDir, gpu);
        del.viewer = v;
        NSWindow *win = v.window;
        del.window = win;
        if (full) [win toggleFullScreen:nil];
        if (slide_ms > 0) [v startSlideshow:slide_ms];
        if (auto_ms > 0) {
            __block NSInteger shown = 1;
            [NSTimer scheduledTimerWithTimeInterval:auto_ms / 1000.0 repeats:YES block:^(NSTimer *t) {
                shown++;
                if (!v.lazyDir && v.index >= (NSInteger)v.files.count - 1) { [NSApp terminate:nil]; return; }
                const char *tk = getenv("NV_TEST_KEYS");   // test: "N:keys,..." at tick N press keys (no paging)
                if (tk) {
                    char pat[16];
                    snprintf(pat, sizeof pat, "%ld:", (long)shown);
                    const char *m = strstr(tk, pat);
                    if (m && (m == tk || m[-1] == ',')) {
                        Decoded *d = [v.loader get:v.index];
                        for (const char *k = m + strlen(pat); d && *k && *k != ','; k++) {
                            if (*k == 'W') [v windowToImage];
                            else [v testKey:*k image:d view:v.drawableSize];
                        }
                        return;
                    }
                }
                if (getenv("NV_TEST_RESIZE")) {   // test: "user" resize before image 2, W before image 4
                    if (shown == 2) [win setFrame:[win frameRectForContentRect:NSMakeRect(200, 200, 1000, 700)] display:YES];
                    if (shown == 4) [v windowToImage];
                }
                if (shown == 2 && getenv("NV_TEST_ZOOM")) {   // test: zoom to 100% before paging on
                    Decoded *d = [v.loader get:v.index];
                    if (d) [v testKey:'1' image:d view:v.drawableSize];
                }
                [v page:1];
            }];
        }
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
