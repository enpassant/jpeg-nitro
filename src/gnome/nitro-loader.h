// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro-loader: decodes images on a background thread, the current one first, then
// AHEAD in the paging direction and BEHIND, like the macOS / Windows viewers; so paging
// usually only has to draw. Plain GLib + nitro-image.
#pragma once
#include <gio/gio.h>

#include "nitro-image.h"

G_BEGIN_DECLS

typedef struct _NitroLoader NitroLoader;

// Called on the main thread when image 'index' is decoded (or failed).
typedef void (*NitroLoaderReady)(gpointer user_data, int index);

NitroLoader *nitro_loader_new(NitroLoaderReady ready, gpointer user_data);
void nitro_loader_free(NitroLoader *loader);   // stops (joins) the thread

// A new file list (a GPtrArray of GFile; the loader keeps a reference). The decoded
// image of keep_index in the old list, if any, is kept as new_index (-1: none).
void nitro_loader_set_files(NitroLoader *loader, GPtrArray *files, int keep_index, int new_index);

// File i left the list (deleted) / a file came back at i (undo): the decoded images move with
// their files. (The list is the one given to nitro_loader_set_files: the caller sees the change.)
void nitro_loader_remove_file(NitroLoader *loader, int index);
void nitro_loader_insert_file(NitroLoader *loader, int index, GFile *file);

// The DNG exposure correction changed (nitro_image_set_dng_ev3): DNGs decoded with another one
// are dropped and decoded again.
void nitro_loader_dng_changed(NitroLoader *loader);

// The image shown now and the paging direction (+1 / -1): decides what is decoded next.
void nitro_loader_focus(NitroLoader *loader, int index, int direction);

// A new NitroImage (own texture / SVG references, free with nitro_image_free) or NULL if not
// decoded yet. Main thread.
NitroImage *nitro_loader_get(NitroLoader *loader, int index);

// TRUE if the image could not be decoded; *message (optional) gets a copy of the reason.
gboolean nitro_loader_failed(NitroLoader *loader, int index, char **message);

G_END_DECLS
