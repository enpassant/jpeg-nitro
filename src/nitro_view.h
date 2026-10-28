// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_view.h: the zoom / fit / pan arithmetic the four viewers share (header-only). Sizes
// are in device pixels: the image's as displayed (after its EXIF orientation), the view's. The
// viewers keep their own state (zoomed, scale, the image point at the view centre) and window
// handling; these are the rules all of them apply to it.
#pragma once
#include <math.h>

// The image's size as displayed: EXIF orientations 5..8 turn it by 90 degrees.
static inline void nv_display_size(int w, int h, int orientation, double *iw, double *ih) {
    int swap = orientation >= 5;
    *iw = swap ? h : w;
    *ih = swap ? w : h;
}

// The scale that fits an image of iw x ih into a view of vw x vh; small images stay at 100%
// unless upscale (full screen, or key 0: fit to the screen).
static inline double nv_fit_scale(double iw, double ih, double vw, double vh, int upscale) {
    double s = vw / iw < vh / ih ? vw / iw : vh / ih;
    if (s > 1 && !upscale) s = 1;
    return s;
}

// The window content size for an image of w x h (in the units of the maximum mw x mh: pixels,
// or points on macOS): smaller than the screen: exactly 100% (unless upscale); larger: its
// aspect ratio, as large as fits. At least 320 x 200.
static inline void nv_content_size(double w, double h, double mw, double mh, int upscale, double *cw, double *ch) {
    double k = mw / w < mh / h ? mw / w : mh / h;
    if (!upscale && k > 1.0) k = 1.0;
    double x = k == 1 ? ceil(w) : floor(w * k), y = k == 1 ? ceil(h) : floor(h * k);
    *cw = x > 320 ? x : 320;
    *ch = y > 200 ? y : 200;
}

// The "fit" zoom used for snapping: that of the normal image-sized window (content cw x ch, in
// the image's units); the current window may have shrunk with a zoomed-out image.
static inline double nv_fit_snap_scale(double iw, double ih, double cw, double ch, int fit_screen) {
    double s = cw / iw < ch / ih ? cw / iw : ch / ih;
    return fit_screen || s < 1.0 ? s : 1.0;
}

// Keeps the view centre (cx, cy: an image point) inside the image at scale s, or centred
// where the image is smaller than the view.
static inline void nv_clamp_center(double iw, double ih, double vw, double vh, double s, double *cx, double *cy) {
    double w = vw / s, h = vh / s;
    if (iw <= w) *cx = iw / 2;
    else *cx = *cx < w / 2 ? w / 2 : *cx > iw - w / 2 ? iw - w / 2 : *cx;
    if (ih <= h) *cy = ih / 2;
    else *cy = *cy < h / 2 ? h / 2 : *cy > ih - h / 2 ? ih - h / 2 : *cy;
}

// The view centre after zooming from scale cur to ns around the anchor (ax, ay: device pixels
// from the view centre, y down): the image point under the anchor stays under it.
static inline void nv_zoom_anchor(double cur, double ns, double ax, double ay, double *cx, double *cy) {
    double px = *cx + ax / cur, py = *cy + ay / cur;   // image point under the anchor
    *cx = px - ax / ns;
    *cy = py - ay / ns;
}

// The scale after a zoom step by f from cur (fit: the snapping fit scale): a step crossing
// 100% or fit stops exactly there; between fit / 4 (or 25%) and 3200%; sqrt(2)^2 is exactly
// 200%; a step landing within 2% of fit is fit - except continuous steps (a touchpad's many
// small ones: the snap would swallow them).
static inline double nv_zoom_step(double cur, double fit, double f, int continuous) {
    double ns = cur * f;
    for (int k = 0; k < 2; k++) {
        double snap = k == 0 ? 1.0 : fit;
        if ((cur < snap - 1e-9 && ns > snap + 1e-9) || (cur > snap + 1e-9 && ns < snap - 1e-9)) {
            ns = snap;
            break;
        }
    }
    double lo = (fit < 1.0 ? fit : 1.0) / 4;
    if (ns > 32.0) ns = 32.0;
    if (ns < lo) ns = lo;
    double r = round(ns);
    if (r >= 1 && fabs(ns - r) < 1e-6 * r) ns = r;
    if (!continuous && fabs(ns - fit) < 0.02 * fit) ns = fit;
    return ns;
}
