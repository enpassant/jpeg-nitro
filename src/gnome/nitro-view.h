// SPDX-License-Identifier: MIT
#pragma once

#include <gtk/gtk.h>

#include "nitro-image.h"

G_BEGIN_DECLS

#define NITRO_TYPE_VIEW (nitro_view_get_type())
G_DECLARE_FINAL_TYPE(NitroView, nitro_view, NITRO, VIEW, GtkWidget)

NitroView *nitro_view_new(void);
// texture: the pixels; svg (or NULL): its SVG document, svg_scale texture pixels per SVG unit;
// orientation: EXIF 1..8
void nitro_view_set_image(NitroView *self, GdkTexture *texture, NitroSvg *svg, double svg_scale, int orientation);
void nitro_view_clear(NitroView *self);
void nitro_view_fit(NitroView *self, gboolean fit_screen);
void nitro_view_set_fullscreen(NitroView *self, gboolean fullscreen);
void nitro_view_set_user_sized(NitroView *self, gboolean user_sized);
void nitro_view_zoom_to(NitroView *self, double scale, double x, double y);
void nitro_view_zoom_by(NitroView *self, double factor, double x, double y, gboolean continuous);
void nitro_view_toggle_actual_size(NitroView *self, double x, double y);
void nitro_view_pan_pixels(NitroView *self, double dx, double dy);
void nitro_view_pan_fraction(NitroView *self, double fx, double fy);

gboolean nitro_view_is_zoomed(NitroView *self);
gboolean nitro_view_get_fit_screen(NitroView *self);
double nitro_view_get_scale(NitroView *self);
int nitro_view_get_image_width(NitroView *self);
int nitro_view_get_image_height(NitroView *self);

G_END_DECLS
