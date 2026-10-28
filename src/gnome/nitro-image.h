// SPDX-License-Identifier: MIT
#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

// A vector document, SVG (nitrosvg) or CorelDRAW (nitrocdr), shared by the images made from it
// (the pages of a drawing): rendered again, sharp, when zoomed in (see nitro-view.c).
typedef struct _NitroVector NitroVector;

typedef struct {
    GdkTexture *texture;   // the pixels (a vector drawing: rendered at a fixed size)
    NitroVector *vector;   // a vector drawing: its document (NULL otherwise) ...
    int page;              // ... the page shown (from 0) ...
    double vector_scale;   // ... and texture pixels per document unit
    int orientation;       // EXIF orientation (1..8), applied when drawing
    int width;             // display size (after the orientation)
    int height;
    int source_width;
    int source_height;
    int dng, ev3;          // a DNG developed by nitrodng, with this exposure correction (1/3 EV)
    double decode_ms;
    unsigned serial;       // (from nitro_loader_get: the cached image this was copied from)
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
NitroVector *nitro_vector_ref(NitroVector *vector);
void nitro_vector_unref(NitroVector *vector);
int nitro_vector_pages(const NitroVector *vector);
// Renders w x h pixels of a page: pixel (i, j) shows texture point ((x0 + i + 0.5) / scale,
// (y0 + j + 0.5) / scale) of an image of vector_scale texture pixels per document unit; over white.
// NULL on failure. Any thread.
GdkTexture *nitro_vector_render(NitroVector *vector, int page, double vector_scale, int w, int h, double scale, double x0,
                                double y0);
// Another page of a drawing, as an image (rendered as nitro_image_load does). Any thread.
NitroImage *nitro_image_load_page(NitroVector *vector, int page);
NitroImage *nitro_image_load(GFile *file, GError **error);
void nitro_image_free(NitroImage *image);
