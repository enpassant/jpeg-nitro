// SPDX-License-Identifier: MIT
#include "nitro-window.h"
#include "nitro-image.h"
#include "nitro-loader.h"
#include "nitro-view.h"

#include <math.h>
#include <string.h>

#define NITRO_SQRT2 1.4142135623730950488
#define NITRO_INV_SQRT2 0.7071067811865475244

struct _NitroWindow {
    AdwApplicationWindow parent_instance;

    GtkStack *stack;
    NitroView *view;
    AdwStatusPage *status_page;
    AdwHeaderBar *header_bar;

    GPtrArray *files;
    gint index;
    gboolean started;
    gboolean lazy_directory;
    NitroImage *image;             // the image on screen (NULL: loading or failed)
    NitroLoader *loader;           // decodes in the background, with prefetch
    gboolean loading;              // the current image is not decoded yet (the old one stays visible)

    guint slideshow_source;
    int slideshow_ms;
    gboolean slideshow_paused;
    gint64 shown_time;

    double pointer_x;
    double pointer_y;
    gboolean pointer_valid;
    double drag_last_x;
    double drag_last_y;
    double page_accum;
    double pinch_last_scale;

    GPtrArray *deleted;            // Del: for undo (DeletedFile)
    gboolean sized;                // the window was sized to the first image
    guint present_source;          // shows the window if the first image takes long
};

typedef struct {
    GFile *original, *moved;       // where it was, where it is now (in .delete)
    int index;                     // its place in the list
} DeletedFile;

static void deleted_file_free(gpointer data) {
    DeletedFile *d = data;
    g_object_unref(d->original);
    g_object_unref(d->moved);
    g_free(d);
}

G_DEFINE_FINAL_TYPE(NitroWindow, nitro_window, ADW_TYPE_APPLICATION_WINDOW)

static void update_title(NitroWindow *self);
static void schedule_slideshow(NitroWindow *self);
static void page(NitroWindow *self, gint delta);
static void size_window(NitroWindow *self, gboolean fill);

static gboolean supported_extension(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || !dot[1]) return FALSE;
    dot++;
    // our decoders, SVG (nitrosvg), CorelDRAW (nitrocdr) and GTK fallback loaders (see nitro-image.c)
    static const char *const exts[] = {"jpg", "jpeg", "jpe", "jfif", "png", "psd", "psb", "heic", "heif", "avif",
                                       "dng", "cr2", "nef", "nrw", "arw", "raf",
                                       "tif", "tiff", "gif", "bmp", "webp", "svg",
#ifdef NV_CDR
                                       "cdr", "cmx",
#endif
                                       NULL};
    for (int i = 0; exts[i]; i++)
        if (g_ascii_strcasecmp(dot, exts[i]) == 0) return TRUE;
    return FALSE;
}

static gint compare_files(gconstpointer a, gconstpointer b) {
    GFile *fa = *(GFile * const *)a;
    GFile *fb = *(GFile * const *)b;
    char *na = g_file_get_basename(fa);
    char *nb = g_file_get_basename(fb);
    char *da = g_filename_display_name(na ? na : "");
    char *db = g_filename_display_name(nb ? nb : "");
    char *ka = g_utf8_collate_key_for_filename(da, -1);
    char *kb = g_utf8_collate_key_for_filename(db, -1);
    gint result = strcmp(ka, kb);
    g_free(ka);
    g_free(kb);
    g_free(da);
    g_free(db);
    g_free(na);
    g_free(nb);
    return result;
}

static GPtrArray *collect_directory(GFile *directory) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    GError *error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children(
        directory,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, &error);

    if (!enumerator) {
        g_clear_error(&error);
        return out;
    }

    for (;;) {
        GFileInfo *info = g_file_enumerator_next_file(enumerator, NULL, &error);
        if (!info) break;
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) {
            const char *name = g_file_info_get_name(info);
            if (supported_extension(name))
                g_ptr_array_add(out, g_file_get_child(directory, name));
        }
        g_object_unref(info);
    }

    g_clear_error(&error);
    g_object_unref(enumerator);
    g_ptr_array_sort(out, compare_files);
    return out;
}

static GPtrArray *collect_inputs(GFile **files, gint n_files) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);

    for (gint i = 0; i < n_files; i++) {
        if (!files[i]) continue;
        GFileType type = g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL);
        if (type == G_FILE_TYPE_DIRECTORY) {
            GPtrArray *dir = collect_directory(files[i]);
            for (guint j = 0; j < dir->len; j++)
                g_ptr_array_add(out, g_object_ref(g_ptr_array_index(dir, j)));
            g_ptr_array_unref(dir);
        } else {
            g_ptr_array_add(out, g_object_ref(files[i]));
        }
    }

    return out;
}

static void cancel_slideshow(NitroWindow *self) {
    if (self->slideshow_source) {
        g_source_remove(self->slideshow_source);
        self->slideshow_source = 0;
    }
}

static void set_status(NitroWindow *self, const char *title, const char *description) {
    nitro_view_clear(self->view);
    adw_status_page_set_title(self->status_page, title);
    adw_status_page_set_description(self->status_page, description);
    gtk_stack_set_visible_child(self->stack, GTK_WIDGET(self->status_page));
}

static void update_title(NitroWindow *self) {
    if (!self->files || self->files->len == 0 || self->index < 0 || self->index >= (gint)self->files->len) {
        gtk_window_set_title(GTK_WINDOW(self), "NitroView");
        return;
    }

    GFile *file = g_ptr_array_index(self->files, self->index);
    char *basename = g_file_get_basename(file);
    char *name = g_filename_display_name(basename ? basename : "NitroView");
    char *title;

    if (self->image) {
        double scale = nitro_view_get_scale(self->view);
        title = g_strdup_printf("%s  %.0f%%%s  (%d/%u)  %dx%d  decode %.1f ms",
                                name, scale * 100.0,
                                nitro_view_is_zoomed(self->view) ? "" : " (fit)",
                                self->index + 1, self->files->len,
                                self->image->source_width, self->image->source_height,
                                self->image->decode_ms);
    } else {
        title = g_strdup_printf("%s  (%d/%u)  %s", name, self->index + 1, self->files->len,
                                self->loading ? "loading..." : "CANNOT DECODE (unsupported or damaged)");
    }

    int pages = self->image ? nitro_vector_pages(self->image->vector) : 0;
    if (pages > 1) {
        char *with_page = g_strdup_printf("%s  [page %d/%d]", title, self->image->page + 1, pages);
        g_free(title);
        title = with_page;
    }

    if (self->image && self->image->dng && self->image->ev3) {
        char *with_ev = g_strdup_printf("%s  %+.1f EV", title, self->image->ev3 / 3.0);
        g_free(title);
        title = with_ev;
    }

    if (self->slideshow_ms > 0) {
        char *suffix = self->slideshow_paused
            ? g_strdup("[slideshow paused: P]")
            : g_strdup_printf("[slideshow %d ms]", self->slideshow_ms);
        char *with_slideshow = g_strdup_printf("%s   %s", title, suffix);
        g_free(suffix);
        g_free(title);
        title = with_slideshow;
    }

    gtk_window_set_title(GTK_WINDOW(self), title);
    g_free(title);
    g_free(name);
    g_free(basename);
}

static void expand_directory(NitroWindow *self) {
    if (!self->lazy_directory || !self->files || self->files->len == 0) return;
    self->lazy_directory = FALSE;

    GFile *current = g_ptr_array_index(self->files, self->index);
    GFile *parent = g_file_get_parent(current);
    if (!parent) return;

    GPtrArray *list = collect_directory(parent);
    g_object_unref(parent);
    if (list->len < 2) {
        g_ptr_array_unref(list);
        return;
    }

    gint found = -1;
    for (guint i = 0; i < list->len; i++) {
        if (g_file_equal(current, g_ptr_array_index(list, i))) {
            found = (gint)i;
            break;
        }
    }
    if (found < 0) {
        g_ptr_array_unref(list);
        return;
    }

    nitro_loader_set_files(self->loader, list, self->index, found);   // the decoded image stays
    g_ptr_array_unref(self->files);
    self->files = list;
    self->index = found;
    update_title(self);
}

static gboolean slideshow_step(gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    self->slideshow_source = 0;
    if (!self->slideshow_paused && self->slideshow_ms > 0 &&
        self->files && self->index < (gint)self->files->len - 1)
        page(self, 1);
    return G_SOURCE_REMOVE;
}

static void schedule_slideshow(NitroWindow *self) {
    cancel_slideshow(self);
    if (self->slideshow_ms <= 0 || self->slideshow_paused || !self->files) return;

    expand_directory(self);
    if (self->index >= (gint)self->files->len - 1) return;

    gint64 elapsed_ms = self->shown_time > 0 ? (g_get_monotonic_time() - self->shown_time) / 1000 : 0;
    guint delay = (guint)MAX(1, self->slideshow_ms - elapsed_ms);
    self->slideshow_source = g_timeout_add(delay, slideshow_step, self);
}

// Shows the current image if the loader has it; otherwise the previous one stays on screen
// (title: loading...) until image_ready() comes.
static void show_current(NitroWindow *self) {
    if (!self->files || self->files->len == 0) return;

    NitroImage *image = nitro_loader_get(self->loader, self->index);
    char *message = NULL;
    if (!image && !nitro_loader_failed(self->loader, self->index, &message)) {
        nitro_image_free(self->image);
        self->image = NULL;
        self->loading = TRUE;
        update_title(self);
        return;
    }

    cancel_slideshow(self);
    nitro_image_free(self->image);
    self->image = image;
    self->loading = FALSE;

    if (!image) {
        set_status(self, "Unable to open image", message ? message : "Unknown decode error");
        g_free(message);
        update_title(self);
        self->shown_time = g_get_monotonic_time();
        schedule_slideshow(self);
        return;
    }

    nitro_view_set_image(self->view, image->texture, image->vector, image->page, image->vector_scale, image->orientation);
    if (!self->sized) {   // the first image: the window to its size, then shown
        self->sized = TRUE;
        size_window(self, FALSE);
        if (self->present_source) {
            g_clear_handle_id(&self->present_source, g_source_remove);
            gtk_window_present(GTK_WINDOW(self));
        }
    }
    gtk_stack_set_visible_child(self->stack, GTK_WIDGET(self->view));
    update_title(self);
    self->shown_time = g_get_monotonic_time();
    schedule_slideshow(self);
}

static void image_ready(gpointer user_data, int index) {   // from the loader, main thread
    NitroWindow *self = NITRO_WINDOW(user_data);
    if (index == self->index && !self->image) show_current(self);
}

static void go_to(NitroWindow *self, gint index, gint direction) {
    if (!self->files || self->files->len == 0) return;
    index = CLAMP(index, 0, (gint)self->files->len - 1);
    if (self->started && index == self->index) return;
    self->started = TRUE;
    self->index = index;
    nitro_loader_focus(self->loader, index, direction);   // decode this one next, then its neighbours
    show_current(self);
}

static void page(NitroWindow *self, gint delta) {
    expand_directory(self);
    go_to(self, self->index + delta, delta >= 0 ? 1 : -1);
}

static void page_first(NitroWindow *self) {
    expand_directory(self);
    go_to(self, 0, 1);
}

static void page_last(NitroWindow *self) {
    expand_directory(self);
    if (self->files && self->files->len) go_to(self, (gint)self->files->len - 1, -1);
}

// Del: the current file goes to the ".delete" folder next to it: nothing is lost (undo with U /
// Ctrl+Z, or move it back by hand). The next image follows; the last one is not deleted.
static void delete_current(NitroWindow *self) {
    expand_directory(self);
    if (!self->files || self->files->len < 2) {
        gtk_widget_error_bell(GTK_WIDGET(self));
        return;
    }
    GFile *file = g_ptr_array_index(self->files, self->index);
    GFile *parent = g_file_get_parent(file);
    if (!parent) {
        gtk_widget_error_bell(GTK_WIDGET(self));
        return;
    }
    GFile *bin = g_file_get_child(parent, ".delete");
    g_file_make_directory(bin, NULL, NULL);   // (or it is there already)
    char *name = g_file_get_basename(file);
    GFile *to = g_file_get_child(bin, name);
    for (int n = 2; g_file_query_exists(to, NULL); n++) {   // (never over an earlier one)
        const char *dot = strrchr(name, '.');
        char *alt = dot && dot != name ? g_strdup_printf("%.*s (%d)%s", (int)(dot - name), name, n, dot)
                                       : g_strdup_printf("%s (%d)", name, n);
        g_object_unref(to);
        to = g_file_get_child(bin, alt);
        g_free(alt);
    }
    GError *error = NULL;
    if (!g_file_move(file, to, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &error)) {
        g_warning("cannot move %s: %s", name, error->message);
        g_clear_error(&error);
        gtk_widget_error_bell(GTK_WIDGET(self));
        g_object_unref(to);
    } else {
        DeletedFile *d = g_new(DeletedFile, 1);
        d->original = g_object_ref(file);
        d->moved = to;
        d->index = self->index;
        g_ptr_array_add(self->deleted, d);
        gint i = self->index;
        nitro_loader_remove_file(self->loader, i);   // (from self->files too: the same list)
        self->started = FALSE;
        go_to(self, MIN(i, (gint)self->files->len - 1), 1);
    }
    g_free(name);
    g_object_unref(bin);
    g_object_unref(parent);
}

// Undo the last Del: the file goes back to its place in the folder and in the list.
static void undo_delete(NitroWindow *self) {
    if (!self->deleted->len || !self->files) {
        gtk_widget_error_bell(GTK_WIDGET(self));
        return;
    }
    DeletedFile *d = g_ptr_array_index(self->deleted, self->deleted->len - 1);
    if (g_file_query_exists(d->original, NULL) ||   // (something is there again, or it was moved away)
        !g_file_move(d->moved, d->original, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, NULL)) {
        gtk_widget_error_bell(GTK_WIDGET(self));
        return;
    }
    gint i = MIN(d->index, (gint)self->files->len);
    nitro_loader_insert_file(self->loader, i, d->original);
    g_ptr_array_remove_index(self->deleted, self->deleted->len - 1);
    self->started = FALSE;
    go_to(self, i, 1);
}

// [ / ]: another page of a multi-page drawing (CorelDRAW), drawn on a worker thread; it replaces
// the image when ready
typedef struct {
    NitroVector *vector;
    int page, index;
} PageJob;

static void page_job_free(gpointer data) {
    PageJob *j = data;
    nitro_vector_unref(j->vector);
    g_free(j);
}

static void page_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
    PageJob *j = data;
    g_task_return_pointer(task, nitro_image_load_page(j->vector, j->page), (GDestroyNotify)nitro_image_free);
}

static void page_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(source);
    PageJob *j = g_task_get_task_data(G_TASK(result));
    NitroImage *image = g_task_propagate_pointer(G_TASK(result), NULL);
    if (image && self->loader && nitro_loader_replace_image(self->loader, j->index, j->vector, image) &&
        j->index == self->index) {   // (zoom and position stay, as when paging images)
        nitro_image_free(self->image);
        self->image = NULL;
        show_current(self);
    }
}

static void show_page(NitroWindow *self, int delta) {
    if (!self->image || !self->image->vector) return;
    int page = self->image->page + delta;
    if (page < 0 || page >= nitro_vector_pages(self->image->vector)) return;
    PageJob *j = g_new0(PageJob, 1);
    j->vector = nitro_vector_ref(self->image->vector);
    j->page = page;
    j->index = self->index;
    GTask *task = g_task_new(self, NULL, page_done, NULL);
    g_task_set_task_data(task, j, page_job_free);
    g_task_run_in_thread(task, page_thread);
    g_object_unref(task);
}

// DNG: brighter / darker by 1/3 EV (up to 3 EV; kept while paging): the DNGs are developed again,
// the current one stays on screen until then.
static void dng_exposure(NitroWindow *self, int delta) {
    if (!self->image || !self->image->dng) return;
    int ev3 = CLAMP(nitro_image_get_dng_ev3() + delta, -9, 9);
    if (ev3 == nitro_image_get_dng_ev3()) return;
    nitro_image_set_dng_ev3(ev3);
    nitro_loader_dng_changed(self->loader);
    show_current(self);
}

static void set_files(NitroWindow *self, GPtrArray *files, gboolean lazy_directory) {
    if (!files || files->len == 0) {
        if (files) g_ptr_array_unref(files);
        set_status(self, "NitroView", "No supported images found.");
        return;
    }

    cancel_slideshow(self);
    nitro_loader_set_files(self->loader, files, -1, 0);
    if (self->files) g_ptr_array_unref(self->files);
    self->files = files;
    self->index = 0;
    self->started = FALSE;
    self->lazy_directory = lazy_directory;
    go_to(self, 0, 1);
}

// The window to the image's size (larger than the screen: as large as fits), or with fill (key 0)
// maximized. (Under Wayland a program cannot move its window, only resize it, through its default
// size in GTK 4: the top-left corner stays; so the first window is shown only when it has its
// size, and fill maximizes it instead of growing it past the screen's bottom.)
static void size_window(NitroWindow *self, gboolean fill) {
    if (!self->image || gtk_window_is_fullscreen(GTK_WINDOW(self))) return;
    if (fill) {
        gtk_window_maximize(GTK_WINDOW(self));
        return;
    }
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(self));
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self));
    GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(display, surface) : NULL;
    if (!monitor) {   // (not shown yet: the first monitor)
        GListModel *monitors = gdk_display_get_monitors(display);
        monitor = g_list_model_get_n_items(monitors) ? g_list_model_get_item(monitors, 0) : NULL;
        if (monitor) g_object_unref(monitor);   // (the list keeps it)
    }
    if (!monitor) return;
    GdkRectangle geo;
    gdk_monitor_get_geometry(monitor, &geo);
    int factor = MAX(1, gtk_widget_get_scale_factor(GTK_WIDGET(self)));
    int bar = gtk_widget_get_visible(GTK_WIDGET(self->header_bar)) ? MAX(gtk_widget_get_height(GTK_WIDGET(self->header_bar)), 46) : 0;
    // (GTK 4 tells no work area: room for the panels and the dock)
    double maxw = geo.width * 0.85, maxh = geo.height * 0.85 - bar;
    double iw = (double)self->image->width / factor, ih = (double)self->image->height / factor;   // (100%: pixel for pixel)
    double k = MIN(1.0, MIN(maxw / iw, maxh / ih));
    if (gtk_window_is_maximized(GTK_WINDOW(self))) gtk_window_unmaximize(GTK_WINDOW(self));
    gtk_window_set_default_size(GTK_WINDOW(self), MAX(320, (int)floor(iw * k)), MAX(200, (int)floor(ih * k)) + bar);
}

static void toggle_fullscreen(NitroWindow *self) {
    if (gtk_window_is_fullscreen(GTK_WINDOW(self))) gtk_window_unfullscreen(GTK_WINDOW(self));
    else gtk_window_fullscreen(GTK_WINDOW(self));
}

static void window_to_image(NitroWindow *self) {
    nitro_view_fit(self->view, FALSE);
    size_window(self, FALSE);
    update_title(self);
}

static void fit_to_screen(NitroWindow *self) {
    nitro_view_fit(self->view, TRUE);
    size_window(self, TRUE);
    update_title(self);
}

static void toggle_slideshow_pause(NitroWindow *self) {
    if (self->slideshow_ms <= 0) return;
    self->slideshow_paused = !self->slideshow_paused;
    cancel_slideshow(self);
    self->shown_time = g_get_monotonic_time();
    if (!self->slideshow_paused) schedule_slideshow(self);
    update_title(self);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType state, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    double step = shift ? 0.5 : 0.125;
    double cx = gtk_widget_get_width(GTK_WIDGET(self->view)) / 2.0;
    double cy = gtk_widget_get_height(GTK_WIDGET(self->view)) / 2.0;

    switch (keyval) {
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
        nitro_view_zoom_by(self->view, NITRO_SQRT2, cx, cy, FALSE); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_underscore: case GDK_KEY_KP_Subtract:
        nitro_view_zoom_by(self->view, NITRO_INV_SQRT2, cx, cy, FALSE); return TRUE;
    case GDK_KEY_0: case GDK_KEY_KP_0:
        fit_to_screen(self); return TRUE;
    case GDK_KEY_1: case GDK_KEY_KP_1: nitro_view_zoom_to(self->view, 1, cx, cy); return TRUE;
    case GDK_KEY_2: case GDK_KEY_KP_2: nitro_view_zoom_to(self->view, 2, cx, cy); return TRUE;
    case GDK_KEY_3: case GDK_KEY_KP_3: nitro_view_zoom_to(self->view, 3, cx, cy); return TRUE;
    case GDK_KEY_4: case GDK_KEY_KP_4: nitro_view_zoom_to(self->view, 4, cx, cy); return TRUE;
    case GDK_KEY_5: case GDK_KEY_KP_5: nitro_view_zoom_to(self->view, 5, cx, cy); return TRUE;
    case GDK_KEY_6: case GDK_KEY_KP_6: nitro_view_zoom_to(self->view, 6, cx, cy); return TRUE;
    case GDK_KEY_7: case GDK_KEY_KP_7: nitro_view_zoom_to(self->view, 7, cx, cy); return TRUE;
    case GDK_KEY_8: case GDK_KEY_KP_8: nitro_view_zoom_to(self->view, 8, cx, cy); return TRUE;
    case GDK_KEY_Left:  nitro_view_pan_fraction(self->view, -step, 0); return TRUE;
    case GDK_KEY_Right: nitro_view_pan_fraction(self->view,  step, 0); return TRUE;
    case GDK_KEY_Up:    nitro_view_pan_fraction(self->view, 0, -step); return TRUE;
    case GDK_KEY_Down:  nitro_view_pan_fraction(self->view, 0,  step); return TRUE;
    case GDK_KEY_Page_Down: case GDK_KEY_space: page(self, 1); return TRUE;
    case GDK_KEY_Page_Up: case GDK_KEY_BackSpace: page(self, -1); return TRUE;
    case GDK_KEY_Home: page_first(self); return TRUE;
    case GDK_KEY_End: page_last(self); return TRUE;
    case GDK_KEY_f: case GDK_KEY_F: case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        toggle_fullscreen(self); return TRUE;
    case GDK_KEY_Escape:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self))) gtk_window_unfullscreen(GTK_WINDOW(self));
        else g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self))));
        return TRUE;
    case GDK_KEY_q: case GDK_KEY_Q:
        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)))); return TRUE;
    case GDK_KEY_p: case GDK_KEY_P:
        toggle_slideshow_pause(self); return TRUE;
    case GDK_KEY_w: case GDK_KEY_W:
        window_to_image(self); return TRUE;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
        delete_current(self); return TRUE;
    case GDK_KEY_u: case GDK_KEY_U:
        undo_delete(self); return TRUE;
    case GDK_KEY_z: case GDK_KEY_Z:
        if (!(state & GDK_CONTROL_MASK)) return FALSE;
        undo_delete(self); return TRUE;
    case GDK_KEY_e: case GDK_KEY_E:
        dng_exposure(self, shift ? -1 : 1); return TRUE;
    case GDK_KEY_bracketleft:
        show_page(self, -1); return TRUE;
    case GDK_KEY_bracketright:
        show_page(self, 1); return TRUE;
    default:
        return FALSE;
    }
}

static void click_pressed(GtkGestureClick *gesture, gint n_press, double x, double y, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (button == GDK_BUTTON_PRIMARY && n_press == 2) {
        nitro_view_toggle_actual_size(self->view, x, y);
    } else if (button == GDK_BUTTON_SECONDARY && n_press == 1) {
        page(self, (state & GDK_SHIFT_MASK) ? -1 : 1);
    } else if (button == 8 && n_press == 1) {
        page(self, -1);
    } else if (button == 9 && n_press == 1) {
        page(self, 1);
    }
}

static void drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    self->drag_last_x = 0.0;
    self->drag_last_y = 0.0;
    if (nitro_view_is_zoomed(self->view)) gtk_widget_set_cursor_from_name(GTK_WIDGET(self->view), "grabbing");
}

static void drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    nitro_view_pan_pixels(self->view, offset_x - self->drag_last_x, offset_y - self->drag_last_y);
    self->drag_last_x = offset_x;
    self->drag_last_y = offset_y;
}

static void drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    gtk_widget_set_cursor(GTK_WIDGET(self->view), NULL);
}

static void pointer_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    self->pointer_x = x;
    self->pointer_y = y;
    self->pointer_valid = TRUE;
}

static void pointer_leave(GtkEventControllerMotion *controller, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    self->pointer_valid = FALSE;
}

static void scroll_begin(GtkEventControllerScroll *controller, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    self->page_accum = 0.0;
}

static gboolean scroll_cb(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
#if GTK_CHECK_VERSION(4, 8, 0)
    gboolean wheel = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL;
#else
    gboolean wheel = TRUE;   // (GTK 4.6 does not tell a wheel from a touchpad: steps)
#endif

    if (state & GDK_CONTROL_MASK) {
        if (wheel) {
            if (dy > 0) page(self, 1);
            else if (dy < 0) page(self, -1);
        } else {
            self->page_accum += dy;
            while (self->page_accum >= 40.0) { self->page_accum -= 40.0; page(self, 1); }
            while (self->page_accum <= -40.0) { self->page_accum += 40.0; page(self, -1); }
        }
        return TRUE;
    }

    if (dy == 0.0) return FALSE;
    gboolean continuous = !wheel;
    double factor = continuous ? exp(-dy * 0.01) : (dy < 0 ? NITRO_SQRT2 : NITRO_INV_SQRT2);
    double x = self->pointer_valid ? self->pointer_x : gtk_widget_get_width(GTK_WIDGET(self->view)) / 2.0;
    double y = self->pointer_valid ? self->pointer_y : gtk_widget_get_height(GTK_WIDGET(self->view)) / 2.0;
    nitro_view_zoom_by(self->view, factor, x, y, continuous);
    return TRUE;
}

static void pinch_begin(GtkGesture *gesture, GdkEventSequence *sequence, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    self->pinch_last_scale = 1.0;
}

static void pinch_scale_changed(GtkGestureZoom *gesture, double scale, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    if (self->pinch_last_scale <= 0.0) self->pinch_last_scale = 1.0;
    double factor = scale / self->pinch_last_scale;
    self->pinch_last_scale = scale;

    double x = gtk_widget_get_width(GTK_WIDGET(self->view)) / 2.0;
    double y = gtk_widget_get_height(GTK_WIDGET(self->view)) / 2.0;
    gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &x, &y);
    nitro_view_zoom_by(self->view, factor, x, y, TRUE);
}

static gboolean drop_cb(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    GPtrArray *inputs = g_ptr_array_new_with_free_func(g_object_unref);

    if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        GFile *file = g_value_get_object(value);
        if (file) g_ptr_array_add(inputs, g_object_ref(file));
    } else if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *list = g_value_get_boxed(value);
        for (GSList *it = gdk_file_list_get_files(list); it; it = it->next)
            if (G_IS_FILE(it->data)) g_ptr_array_add(inputs, g_object_ref(it->data));
    }

    if (inputs->len == 0) {
        g_ptr_array_unref(inputs);
        return FALSE;
    }

    GPtrArray *files = collect_inputs((GFile **)inputs->pdata, (gint)inputs->len);
    g_ptr_array_unref(inputs);
    set_files(self, files, FALSE);
    return TRUE;
}

static void view_changed(NitroView *view, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    update_title(self);
}

static void fullscreened_notify(GObject *object, GParamSpec *pspec, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    gboolean fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(self));
    nitro_view_set_fullscreen(self->view, fullscreen);
    gtk_widget_set_visible(GTK_WIDGET(self->header_bar), !fullscreen);
    update_title(self);
}

static void open_chosen(NitroWindow *self, GListModel *model) {   // files and folders from the Open dialog
    guint count = g_list_model_get_n_items(model);
    GPtrArray *inputs = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < count; i++) {
        GFile *file = g_list_model_get_item(model, i);
        if (file) g_ptr_array_add(inputs, file);
    }
    if (inputs->len) {
        GPtrArray *files = collect_inputs((GFile **)inputs->pdata, (gint)inputs->len);
        set_files(self, files, FALSE);
    }
    g_ptr_array_unref(inputs);
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void open_dialog_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    NitroWindow *self = NITRO_WINDOW(user_data);
    GError *error = NULL;
    GListModel *model = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, &error);

    if (model) {
        open_chosen(self, model);
        g_object_unref(model);
    } else if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
        g_warning("Open dialog failed: %s", error ? error->message : "unknown error");
    }

    g_clear_error(&error);
    g_object_unref(self);
}
#else
static void chooser_response(GtkNativeDialog *dialog, int response, gpointer user_data) {   // (GTK < 4.10)
    NitroWindow *self = NITRO_WINDOW(user_data);
    if (response == GTK_RESPONSE_ACCEPT) {
        GListModel *model = gtk_file_chooser_get_files(GTK_FILE_CHOOSER(dialog));
        open_chosen(self, model);
        g_object_unref(model);
    }
    g_object_unref(dialog);
    g_object_unref(self);
}
#endif

static void nitro_window_dispose(GObject *object) {
    NitroWindow *self = NITRO_WINDOW(object);
    cancel_slideshow(self);
    g_clear_handle_id(&self->present_source, g_source_remove);
    g_clear_pointer(&self->loader, nitro_loader_free);   // first: no more image_ready() calls
    nitro_image_free(self->image);
    self->image = NULL;
    g_clear_pointer(&self->files, g_ptr_array_unref);
    g_clear_pointer(&self->deleted, g_ptr_array_unref);
#if GTK_CHECK_VERSION(4, 8, 0)
    gtk_widget_dispose_template(GTK_WIDGET(object), NITRO_TYPE_WINDOW);
#endif
    G_OBJECT_CLASS(nitro_window_parent_class)->dispose(object);
}

static void nitro_window_class_init(NitroWindowClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = nitro_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/com/github/gereoffy/nitroview/nitro-window.ui");
    gtk_widget_class_bind_template_child(widget_class, NitroWindow, stack);
    gtk_widget_class_bind_template_child(widget_class, NitroWindow, view);
    gtk_widget_class_bind_template_child(widget_class, NitroWindow, status_page);
    gtk_widget_class_bind_template_child(widget_class, NitroWindow, header_bar);
}

static void nitro_window_init(NitroWindow *self) {
    g_type_ensure(NITRO_TYPE_VIEW);
    gtk_widget_init_template(GTK_WIDGET(self));

    self->index = -1;
    self->loader = nitro_loader_new(image_ready, self);
    self->deleted = g_ptr_array_new_with_free_func(deleted_file_free);
    self->pointer_x = 0.0;
    self->pointer_y = 0.0;
    self->pointer_valid = FALSE;
    self->pinch_last_scale = 1.0;
    nitro_view_set_user_sized(self->view, TRUE);

    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view), GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), self);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), self);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view), GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(pointer_motion), self);
    g_signal_connect(motion, "leave", G_CALLBACK(pointer_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view), motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll-begin", G_CALLBACK(scroll_begin), self);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_cb), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view), scroll);

    GtkGesture *zoom = gtk_gesture_zoom_new();
    g_signal_connect(zoom, "begin", G_CALLBACK(pinch_begin), self);
    g_signal_connect(zoom, "scale-changed", G_CALLBACK(pinch_scale_changed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view), GTK_EVENT_CONTROLLER(zoom));

    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    GType drop_types[] = {G_TYPE_FILE, GDK_TYPE_FILE_LIST};
    gtk_drop_target_set_gtypes(drop, drop_types, G_N_ELEMENTS(drop_types));
    g_signal_connect(drop, "drop", G_CALLBACK(drop_cb), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drop));

    g_signal_connect(self->view, "view-changed", G_CALLBACK(view_changed), self);
    g_signal_connect(self, "notify::fullscreened", G_CALLBACK(fullscreened_notify), self);
}

static gboolean present_anyway(gpointer data) {   // the first image is slow (or failed): shown at the default size
    NitroWindow *self = NITRO_WINDOW(data);
    self->present_source = 0;
    gtk_window_present(GTK_WINDOW(self));
    return G_SOURCE_REMOVE;
}

void nitro_window_present(NitroWindow *self) {
    g_return_if_fail(NITRO_IS_WINDOW(self));
    if (self->sized || !self->files || gtk_widget_get_mapped(GTK_WIDGET(self))) {
        if (!self->present_source) gtk_window_present(GTK_WINDOW(self));
        return;
    }
    if (!self->present_source) self->present_source = g_timeout_add(400, present_anyway, self);
}

NitroWindow *nitro_window_new(AdwApplication *application) {
    return g_object_new(NITRO_TYPE_WINDOW, "application", application, NULL);
}

void nitro_window_open(NitroWindow *self, GFile *file) {
    g_return_if_fail(NITRO_IS_WINDOW(self));
    g_return_if_fail(G_IS_FILE(file));
    GFile *files[] = {file};
    nitro_window_open_files(self, files, 1);
}

void nitro_window_open_files(NitroWindow *self, GFile **files, gint n_files) {
    g_return_if_fail(NITRO_IS_WINDOW(self));
    if (!files || n_files <= 0) return;

    gboolean lazy = n_files == 1 &&
        g_file_query_file_type(files[0], G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY;
    GPtrArray *collected;
    if (lazy) {
        collected = g_ptr_array_new_with_free_func(g_object_unref);
        g_ptr_array_add(collected, g_object_ref(files[0]));
    } else {
        collected = collect_inputs(files, n_files);
    }
    set_files(self, collected, lazy);
}

void nitro_window_open_dialog(NitroWindow *self) {
    g_return_if_fail(NITRO_IS_WINDOW(self));

#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Images");
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL, open_dialog_done, g_object_ref(self));
    g_object_unref(dialog);
#else
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Open Images", GTK_WINDOW(self),
                                                                GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(chooser), TRUE);
    g_signal_connect(chooser, "response", G_CALLBACK(chooser_response), g_object_ref(self));
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
#endif
}

void nitro_window_set_options(NitroWindow *self, gboolean fullscreen, int slideshow_ms) {
    g_return_if_fail(NITRO_IS_WINDOW(self));
    self->slideshow_ms = MAX(slideshow_ms, 0);
    self->slideshow_paused = FALSE;
    if (fullscreen) {
        nitro_view_set_fullscreen(self->view, TRUE);
        gtk_widget_set_visible(GTK_WIDGET(self->header_bar), FALSE);
        gtk_window_fullscreen(GTK_WINDOW(self));
    }
    if (self->image) {
        self->shown_time = g_get_monotonic_time();
        schedule_slideshow(self);
        update_title(self);
    }
}
