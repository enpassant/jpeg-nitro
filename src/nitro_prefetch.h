// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_prefetch: the viewers' image cache and background decoder, shared by nitroview.m,
// nitroview_sdl.c, nitroview_win.c and src/gnome. A thread decodes the current image first,
// then NV_AHEAD images in the paging direction and NV_BEHIND behind it, so paging usually only
// has to draw; what falls out of that window is evicted. Paths and decoded images are the
// viewer's (void *): it gives the callbacks that decode, copy paths and tell the main thread.
// Evicted images are not freed here (textures live on the main thread): the main thread takes
// them with nv_loader_reap and frees them.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_AHEAD 3    // images decoded ahead in the paging direction
#define NV_BEHIND 2   // ... and kept / decoded behind
#define NV_CACHE 16   // decoded images kept at most

typedef struct NvLoader NvLoader;

typedef struct {
    // Decoder thread: the image of a path, NULL if it cannot be decoded (*error, optional: why,
    // from malloc / strdup; the loader frees it).
    void *(*decode)(void *path, char **error, void *ctx);
    // Frees an image (only nv_loader_stop uses it; evicted images go through nv_loader_reap).
    void (*free_image)(void *image, void *ctx);
    // Was the image decoded with settings since changed (a DNG's exposure correction)? Such an
    // image is dropped when it arrives, or is replaced (see nv_loader_drop). May be NULL.
    int (*stale)(const void *image, void *ctx);
    // A copy (or another reference) of a path: decoding reads it with the lock released, while
    // the list may change. And freeing a path, and its name for messages (UTF-8, or NULL).
    void *(*path_dup)(void *path, void *ctx);
    void (*path_free)(void *path, void *ctx);
    const char *(*path_name)(void *path, void *ctx);
    // Decoder thread -> the main thread: image index is decoded (or failed: nv_loader_failed), or
    // -1: nothing new, but there are evicted images to reap.
    void (*notify)(int index, void *ctx);
    // Called on the decoder thread when it starts (its priority); may be NULL.
    void (*thread_started)(void *ctx);
    void *ctx;
} NvLoaderOps;

// Starts the decoder thread with the file list (takes it: the array from malloc, the paths freed
// with ops->path_free). NULL if out of memory.
NvLoader *nv_loader_new(const NvLoaderOps *ops, void **files, int n);
// Stops the thread (waits for a decode in progress) and frees everything: the images (with
// ops->free_image), the list. The viewers that quit with the process never call it.
void nv_loader_stop(NvLoader *L);

// The decoded image of file i, NULL if not decoded (yet); *serial (optional): its identity, for
// nv_loader_replace_image. Main thread.
void *nv_loader_get(NvLoader *L, int i, unsigned *serial);
// Could file i not be decoded? *error (optional): a copy of the reason (free it), NULL if none.
int nv_loader_failed(NvLoader *L, int i, char **error);
// The image shown now and the paging direction (+1 / -1): decides what is decoded next.
void nv_loader_focus(NvLoader *L, int cur, int dir);

// A new file list (takes it, frees the old one); the decoded image of old_index (if any, -1:
// none) in the old list is kept as new_index.
void nv_loader_replace(NvLoader *L, void **files, int n, int old_index, int new_index);
// File i left the list (deleted): later images move up one place, decoded ones with them.
void nv_loader_remove(NvLoader *L, int i);
// A file came back at i (undo; takes path). -1 if out of memory (path not taken).
int nv_loader_insert(NvLoader *L, int i, void *path);
// The list as it is now (*n: its length): the viewer's list is this same array. Main thread.
void **nv_loader_files(NvLoader *L, int *n);

// An image decoded again (another page of a document, a DNG developed again): image replaces
// the one with that serial if it is still in the cache - wherever it is now (remove / insert
// may have moved it) - and is not stale. Returns its index, -1 if it did not (image evicted).
int nv_loader_replace_image(NvLoader *L, unsigned serial, void *image);
// Drops (evicts) the cached images pred says so of, except the current image when keep_current:
// then, if the current one matches, returns 1 with its serial and a copy of its path (for a
// decode that nv_loader_replace_image puts in its place), else 0. The dropped ones are decoded
// again when they come into the window.
int nv_loader_drop(NvLoader *L, int (*pred)(const void *image, void *ctx), void *ctx, int keep_current,
                   unsigned *cur_serial, void **cur_path);
// The evicted images (up to max of them into dead): their count; the caller frees them. Main thread.
int nv_loader_reap(NvLoader *L, void **dead, int max);

#ifdef __cplusplus
}
#endif
