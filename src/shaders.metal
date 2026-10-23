// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroview's Metal shaders, compiled at startup (embedded by the Makefile as build/shaders.inc).
#include <metal_stdlib>
using namespace metal;

struct ConvParams {
    uint2 size;        // output size
    float2 cscale;     // luma pixel -> chroma pixel scale (h_c/hmax, v_c/vmax)
    float2 csize;      // chroma plane size
    uint kind;         // 0 YCbCr, 1 gray, 2 BGRA copy, 5 YCbCr limited range
};

kernel void convert(texture2d<float, access::read> Y [[texture(0)]],
                    texture2d<float, access::sample> Cb [[texture(1)]],
                    texture2d<float, access::sample> Cr [[texture(2)]],
                    texture2d<float, access::write> out [[texture(3)]],
                    constant ConvParams &p [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.size.x || gid.y >= p.size.y) return;
    float4 c;
    if (p.kind == 2) {
        c = float4(Y.read(gid).rgb, 1.0);
    } else {
        float y = Y.read(gid).r;
        if (p.kind == 1) {
            c = float4(y, y, y, 1.0);
        } else {
            constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
            // chroma sample centres: same "fancy upsampling" as libjpeg (linear, centred)
            float2 uv = (float2(gid) + 0.5) * p.cscale / p.csize;
            float cb = Cb.sample(s, uv).r - 128.0 / 255.0;
            float cr = Cr.sample(s, uv).r - 128.0 / 255.0;
            if (p.kind == 5) {   // limited range (WebP): Y 16..235, C 16..240
                y = (y - 16.0 / 255.0) * (255.0 / 219.0);
                cb *= 255.0 / 224.0;
                cr *= 255.0 / 224.0;
            }
            c = float4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);
        }
    }
    out.write(saturate(c), gid);
}

struct PlaneParams {
    uint2 size;
    float2 cscale;
    int2 cmax;         // chroma plane size - 1
    uint off[3];
    uint pitch[3];
    uint kind;
    uint aoff;         // PSD: offset of the transparency plane (hasa = 1)
    uint hasa;
};

static inline float chroma(device const uchar *p, uint pitch, float2 c, int2 cmax) {
    float2 f = c - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 a = clamp(i0, int2(0), cmax), b = clamp(i0 + 1, int2(0), cmax);
    float v00 = p[a.y * pitch + a.x], v01 = p[a.y * pitch + b.x];
    float v10 = p[b.y * pitch + a.x], v11 = p[b.y * pitch + b.x];
    return mix(mix(v00, v01, w.x), mix(v10, v11, w.x), w.y) * (1.0 / 255.0);
}

kernel void convert_buf(device const uchar *src [[buffer(1)]],
                        texture2d<float, access::write> out [[texture(3)]],
                        constant PlaneParams &p [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.size.x || gid.y >= p.size.y) return;
    float4 c;
    if (p.kind == 2) {
        device const uchar *q = src + gid.y * p.pitch[0] + gid.x * 4;
        c = float4(q[2], q[1], q[0], 255) * (1.0 / 255.0);
    } else if (p.kind == 4) {   // PSD planes (gray: all offsets equal)
        uint i = gid.y * p.pitch[0] + gid.x;
        float3 v = float3(src[p.off[0] + i], src[p.off[1] + i], src[p.off[2] + i]);
        if (p.hasa) v = max(v + float(src[p.aoff + i]) - 255.0, 0.0);   // matted with white -> over black
        c = float4(v * (1.0 / 255.0), 1.0);
    } else if (p.kind == 3) {   // PNG rows (after the filter byte), off[1] = channels; premultiplied over black
        // rows start at odd addresses: read through packed types (1-byte alignment), otherwise
        // the compiler merges the bytes into one aligned load that silently drops the low bits
        uint ch = p.off[1];
        device const uchar *q = src + p.off[0] + gid.y * p.pitch[0] + gid.x * ch;
        float4 v;
        if (ch == 4) v = float4(uchar4(*(device const packed_uchar4 *)q));
        else if (ch == 3) v = float4(float3(uchar3(*(device const packed_uchar3 *)q)), 255);
        else if (ch == 2) { uchar2 t = uchar2(*(device const packed_uchar2 *)q); v = float4(t.x, t.x, t.x, t.y); }
        else v = float4(q[0], q[0], q[0], 255);
        v *= 1.0 / 255.0;
        c = float4(v.rgb * v.a, 1.0);
    } else {
        float y = src[p.off[0] + gid.y * p.pitch[0] + gid.x] * (1.0 / 255.0);
        if (p.kind == 1) {
            c = float4(y, y, y, 1.0);
        } else {
            float2 cc = (float2(gid) + 0.5) * p.cscale;
            float cb = chroma(src + p.off[1], p.pitch[1], cc, p.cmax) - 128.0 / 255.0;
            float cr = chroma(src + p.off[2], p.pitch[2], cc, p.cmax) - 128.0 / 255.0;
            if (p.kind == 5) {   // limited range (WebP): Y 16..235, C 16..240
                y = (y - 16.0 / 255.0) * (255.0 / 219.0);
                cb *= 255.0 / 224.0;
                cr *= 255.0 / 224.0;
            }
            c = float4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);
        }
    }
    out.write(saturate(c), gid);
}

struct VOut { float4 pos [[position]]; float2 uv; };
struct DrawParams { float2 origin; float2 size; uint orientation; uint nearest; };   // NDC top-left + extent

vertex VOut vmain(uint vid [[vertex_id]], constant DrawParams &p [[buffer(0)]]) {
    float2 q = float2(vid & 1, vid >> 1);            // 0..1 display space, y down
    VOut o;
    o.pos = float4(p.origin.x + q.x * p.size.x, p.origin.y - q.y * p.size.y, 0, 1);
    float u = q.x, v = q.y;
    float2 t;
    switch (p.orientation) {                          // EXIF orientation
        case 2: t = float2(1 - u, v); break;
        case 3: t = float2(1 - u, 1 - v); break;
        case 4: t = float2(u, 1 - v); break;
        case 5: t = float2(v, u); break;
        case 6: t = float2(v, 1 - u); break;
        case 7: t = float2(1 - v, 1 - u); break;
        case 8: t = float2(1 - v, u); break;
        default: t = float2(u, v); break;
    }
    o.uv = t;
    return o;
}

fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                      constant DrawParams &p [[buffer(0)]]) {
    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge, max_anisotropy(4));
    constexpr sampler sn(filter::nearest, address::clamp_to_edge);   // integer zoom: exact pixel blocks
    return p.nearest ? tex.sample(sn, in.uv, level(0)) : tex.sample(s, in.uv);
}
