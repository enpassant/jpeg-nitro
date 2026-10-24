// SPDX-License-Identifier: MIT
#include "nitro-window.h"
#include "nitro-image.h"

#define NITRO_APP_ID "com.github.gereoffy.nitroview"
#define NITRO_RESOURCE_BASE "/com/github/gereoffy/nitroview"

static NitroWindow *get_window(AdwApplication *application) {
    GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(application));
    if (active) return NITRO_WINDOW(active);
    return nitro_window_new(application);
}

static void activate_cb(GApplication *application, gpointer user_data) {
    (void)user_data;
    NitroWindow *window = get_window(ADW_APPLICATION(application));
    nitro_window_present(window);
}

static void open_cb(GApplication *application, GFile **files, gint n_files, const gchar *hint, gpointer user_data) {
    (void)hint;
    (void)user_data;

    NitroWindow *window = get_window(ADW_APPLICATION(application));
    if (n_files > 0) nitro_window_open_files(window, files, n_files);
    nitro_window_present(window);
}

static gint command_line_cb(GApplication *application, GApplicationCommandLine *command_line, gpointer user_data) {
    (void)user_data;

    GVariantDict *options = g_application_command_line_get_options_dict(command_line);
    gboolean fullscreen = FALSE;
    gint slideshow_ms = 0;
    gint threads = 0;
    g_variant_dict_lookup(options, "fullscreen", "b", &fullscreen);
    g_variant_dict_lookup(options, "slideshow", "i", &slideshow_ms);
    g_variant_dict_lookup(options, "threads", "i", &threads);

    if (slideshow_ms < 0 || threads < 0) {
        g_application_command_line_printerr(command_line, "slideshow and thread counts must be non-negative\n");
        return 2;
    }

    nitro_image_set_threads(threads);

    NitroWindow *window = get_window(ADW_APPLICATION(application));
    nitro_window_set_options(window, fullscreen, slideshow_ms);

    gint argc = 0;
    gchar **argv = g_application_command_line_get_arguments(command_line, &argc);
    if (argc > 1) {
        GFile **files = g_new0(GFile *, argc - 1);
        for (gint i = 1; i < argc; i++)
            files[i - 1] = g_application_command_line_create_file_for_arg(command_line, argv[i]);
        nitro_window_open_files(window, files, argc - 1);
        for (gint i = 0; i < argc - 1; i++) g_object_unref(files[i]);
        g_free(files);
    }
    g_strfreev(argv);

    nitro_window_present(window);
    return 0;
}

static void open_action_cb(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;

    AdwApplication *application = ADW_APPLICATION(user_data);
    NitroWindow *window = get_window(application);
    nitro_window_present(window);
    nitro_window_open_dialog(window);
}

static void quit_action_cb(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    g_application_quit(G_APPLICATION(user_data));
}

static void startup_cb(GApplication *application, gpointer user_data) {
    (void)user_data;

    static const GActionEntry actions[] = {
        {"open", open_action_cb, NULL, NULL, NULL, {0}},
        {"quit", quit_action_cb, NULL, NULL, NULL, {0}},
    };

    g_action_map_add_action_entries(G_ACTION_MAP(application), actions, G_N_ELEMENTS(actions), application);

    const char *open_accels[] = {"<Primary>o", NULL};
    const char *quit_accels[] = {"<Primary>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(application), "app.open", open_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(application), "app.quit", quit_accels);
}

int main(int argc, char **argv) {
    g_set_application_name("NitroView");

    AdwApplication *application = adw_application_new(
        NITRO_APP_ID,
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_set_resource_base_path(G_APPLICATION(application), NITRO_RESOURCE_BASE);

    g_application_add_main_option(G_APPLICATION(application), "fullscreen", 'f', G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Start in full screen", NULL);
    g_application_add_main_option(G_APPLICATION(application), "slideshow", 's', G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_INT, "Slideshow interval in milliseconds", "MS");
    g_application_add_main_option(G_APPLICATION(application), "threads", 'j', G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_INT, "Limit decoder threads", "N");

    g_signal_connect(application, "startup", G_CALLBACK(startup_cb), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(activate_cb), NULL);
    g_signal_connect(application, "open", G_CALLBACK(open_cb), NULL);
    g_signal_connect(application, "command-line", G_CALLBACK(command_line_cb), NULL);

    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
