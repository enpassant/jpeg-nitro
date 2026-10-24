// SPDX-License-Identifier: MIT
#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

// An SVG document (nitrosvg), shared by the images made from it: rendered again, sharp, when
// zoomed in (see nitro-view.c).
typedef struct _NitroSvg NitroSvg;

typedef struct {
    GdkTexture *texture;   // the pixels (an SVG: rendered at a fixed size)
    NitroSvg *svg;         // an SVG: its document (NULL otherwise)
    double svg_scale;      // an SVG: texture pixels per SVG unit
    int orientation;       // EXIF orientation (1..8), applied when drawing
    int width;             // display size (after the orientation)
    int height;
    int source_width;
    int source_height;
    int dng, ev3;          // a DNG developed by nitrodng, with this exposure correction (1/3 EV)
    double decode_ms;
} NitroImage;

// GTK < 4.10 draws a texture with a plain linear filter, without mipmaps: a photo reduced a lot
// shimmers (moire). So a large image's texture also carries its halves, quarters ... (2x2 box
// filter, object data NITRO_MIPS); the view draws the one at most 2x larger than on screen.
#define NITRO_MIPS "nitro-mips"
typedef struct {
    int n;
    GdkTexture *t[16];
} NitroMips;

void nitro_image_set_threads(int threads);
// DNG exposure correction in 1/3 EV for the images decoded from now on (keys E / Shift+E)
void nitro_image_set_dng_ev3(int ev3);
int nitro_image_get_dng_ev3(void);
NitroSvg *nitro_svg_ref(NitroSvg *svg);
void nitro_svg_unref(NitroSvg *svg);
// Renders w x h pixels of the SVG: pixel (i, j) shows texture point ((x0 + i + 0.5) / scale,
// (y0 + j + 0.5) / scale) of an image of svg_scale texture pixels per SVG unit; over white.
// NULL on failure. Any thread.
GdkTexture *nitro_svg_render(NitroSvg *svg, double svg_scale, int w, int h, double scale, double x0, double y0);
NitroImage *nitro_image_load(GFile *file, GError **error);
void nitro_image_free(NitroImage *image);
