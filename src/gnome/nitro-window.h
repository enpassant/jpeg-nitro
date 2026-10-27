// SPDX-License-Identifier: MIT
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define NITRO_TYPE_WINDOW (nitro_window_get_type())
G_DECLARE_FINAL_TYPE(NitroWindow, nitro_window, NITRO, WINDOW, AdwApplicationWindow)

NitroWindow *nitro_window_new(AdwApplication *application);
void nitro_window_open_files(NitroWindow *self, GFile **files, gint n_files);
void nitro_window_open_dialog(NitroWindow *self);
void nitro_window_set_options(NitroWindow *self, gboolean fullscreen, int slideshow_ms);
// Shows the window: opening images, when the first one is decoded (sized to it) or after 0.4 s.
void nitro_window_present(NitroWindow *self);

G_END_DECLS
