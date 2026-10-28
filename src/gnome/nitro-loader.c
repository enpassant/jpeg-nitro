// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro-loader: background decoding with prefetch. See nitro-loader.h. The cache and the decoder
// thread are nitro_prefetch's (shared with the other viewers); this is its GLib face: GFiles,
// GError messages, notifications on the main loop.
#include "nitro-loader.h"
#include "../nitro_prefetch.h"

#include <stdlib.h>
#include <string.h>

struct _NitroLoader {
    gint refs;                 // the owner + one per pending main-thread notification
    gint quit;                 // set by nitro_loader_free: no more callbacks
    NvLoader *L;
    GPtrArray *files;          // of GFile: the viewer's list, kept in step with the loader's
    NitroLoaderReady ready;
    gpointer user_data;
};

typedef struct {
    NitroLoader *loader;
    int index;                 // -1: nothing to report (only images to free)
} Notify;

static void reap(NitroLoader *l) {   // main thread
    void *dead[64];
    int n;
    while ((n = nv_loader_reap(l->L, dead, 64)) > 0)
        for (int i = 0; i < n; i++) nitro_image_free(dead[i]);
}

static void loader_unref(NitroLoader *l) {   // main thread
    if (!g_atomic_int_dec_and_test(&l->refs)) return;
    g_free(l);
}

static gboolean notify_cb(gpointer data) {
    Notify *n = data;
    NitroLoader *l = n->loader;
    if (!g_atomic_int_get(&l->quit)) {
        reap(l);
        if (n->index >= 0) l->ready(l->user_data, n->index);
    }
    return G_SOURCE_REMOVE;
}

static void notify_free(gpointer data) {
    Notify *n = data;
    loader_unref(n->loader);
    g_free(n);
}

// the callbacks
static void *ld_decode(void *path, char **error, void *ctx) {
    GError *e = NULL;
    NitroImage *image = nitro_image_load(path, &e);
    if (!image) *error = strdup(e ? e->message : "Cannot decode the image");
    g_clear_error(&e);
    return image;
}
static void ld_free(void *image, void *ctx) { nitro_image_free(image); }
static int ld_stale(const void *image, void *ctx) {
    const NitroImage *i = image;
    return i->dng && i->ev3 != nitro_image_get_dng_ev3();
}
static void *ld_path_dup(void *path, void *ctx) { return g_object_ref(path); }
static void ld_path_free(void *path, void *ctx) { g_object_unref(path); }
static const char *ld_path_name(void *path, void *ctx) { return g_file_peek_path(path); }
static void ld_notify(int index, void *ctx) {   // decoder thread -> the main loop
    NitroLoader *l = ctx;
    Notify *n = g_new(Notify, 1);
    n->loader = l;
    n->index = index;
    g_atomic_int_inc(&l->refs);
    g_idle_add_full(G_PRIORITY_DEFAULT, notify_cb, n, notify_free);
}

NitroLoader *nitro_loader_new(NitroLoaderReady ready, gpointer user_data) {
    NitroLoader *l = g_new0(NitroLoader, 1);
    l->refs = 1;
    l->ready = ready;
    l->user_data = user_data;
    NvLoaderOps ops = {ld_decode, ld_free, ld_stale, ld_path_dup, ld_path_free, ld_path_name, ld_notify, NULL, l};
    void **none = malloc(sizeof *none);   // (an empty list until nitro_loader_set_files)
    l->L = none ? nv_loader_new(&ops, none, 0) : NULL;
    if (!l->L) g_error("cannot start the decoder thread");
    return l;
}

void nitro_loader_free(NitroLoader *l) {
    if (!l) return;
    g_atomic_int_set(&l->quit, 1);
    nv_loader_stop(l->L);   // (waits for a decode in progress, frees the images)
    l->L = NULL;
    g_clear_pointer(&l->files, g_ptr_array_unref);
    loader_unref(l);        // pending notifications hold their own references
}

static void **refs_of(GPtrArray *files) {   // the list as the loader takes it
    void **p = malloc(MAX(files ? files->len : 0, 1u) * sizeof *p);
    if (!p) return NULL;
    for (guint i = 0; files && i < files->len; i++) p[i] = g_object_ref(g_ptr_array_index(files, i));
    return p;
}

void nitro_loader_set_files(NitroLoader *l, GPtrArray *files, int keep_index, int new_index) {
    void **p = refs_of(files);
    if (!p) return;
    nv_loader_replace(l->L, p, files ? (int)files->len : 0, keep_index, new_index);
    if (files) g_ptr_array_ref(files);
    if (l->files) g_ptr_array_unref(l->files);
    l->files = files;
}

void nitro_loader_remove_file(NitroLoader *l, int index) {
    if (!l->files || index < 0 || index >= (int)l->files->len) return;
    nv_loader_remove(l->L, index);
    g_ptr_array_remove_index(l->files, (guint)index);
}

void nitro_loader_insert_file(NitroLoader *l, int index, GFile *file) {
    if (!l->files || index < 0 || index > (int)l->files->len) return;
    if (nv_loader_insert(l->L, index, g_object_ref(file))) {
        g_object_unref(file);
        return;
    }
    g_ptr_array_insert(l->files, index, g_object_ref(file));
}

int nitro_loader_replace_image(NitroLoader *l, unsigned serial, NitroImage *image) {
    return nv_loader_replace_image(l->L, serial, image);
}

void nitro_loader_dng_changed(NitroLoader *l) { nv_loader_drop(l->L, ld_stale, NULL, 0, NULL, NULL); }

void nitro_loader_focus(NitroLoader *l, int index, int direction) { nv_loader_focus(l->L, index, direction); }

NitroImage *nitro_loader_get(NitroLoader *l, int index) {
    unsigned serial = 0;
    const NitroImage *image = nv_loader_get(l->L, index, &serial);   // (stays until reaped, which is on this thread)
    if (!image) return NULL;
    NitroImage *copy = g_new(NitroImage, 1);
    *copy = *image;
    if (copy->texture) g_object_ref(copy->texture);
    nitro_vector_ref(copy->vector);
    copy->serial = serial;
    return copy;
}

gboolean nitro_loader_failed(NitroLoader *l, int index, char **message) {
    char *m = NULL;
    int failed = nv_loader_failed(l->L, index, &m);
    if (message) *message = m ? g_strdup(m) : failed ? g_strdup("Cannot decode the image") : NULL;
    free(m);
    return failed != 0;
}
