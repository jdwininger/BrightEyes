/* Main (application entry)
 *
 * Application startup, signal wiring and GResource registration live here.
 * Keeps the entrypoint small and delegates UI construction to window.c.
 */

#include <adwaita.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include "window.h"

/* Compiled GResource accessor (generated) */
GResource *brighteyes_get_resource(void);

static void
startup(GApplication *app, gpointer user_data)
{
    /* Ensure our compiled resource is registered so icon lookups work without installation */
    g_resources_register(brighteyes_get_resource());

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
        gtk_icon_theme_add_resource_path(theme, "/org/brighteyes/BrightEyes/icons");
    }
    gtk_window_set_default_icon_name("org.brightEyes.BrightEyes");
}

static void
activate(GApplication *app, gpointer user_data)
{
    BrightEyesWindow *win = bright_eyes_window_new(GTK_APPLICATION(app));
    gtk_window_present(GTK_WINDOW(win));
}

static void
open(GApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer user_data)
{
    BrightEyesWindow *win;
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    if (windows)
        win = BRIGHT_EYES_WINDOW(windows->data);
    else {
        win = bright_eyes_window_new(GTK_APPLICATION(app));
        gtk_window_present(GTK_WINDOW(win)); 
    }

    if (n_files >= 1) {
        char *path = g_file_get_path(files[0]);
        if (path) {
            bright_eyes_window_open_file(win, path);
            g_free(path);
        }
    }
}

int
main(int argc, char **argv)
{
    /* Use Cairo renderer to avoid OpenGL/Vulkan artifacts (distorted tooltips) */
    g_setenv("GSK_RENDERER", "cairo", FALSE);

    adw_init();
    AdwApplication *app = adw_application_new("org.brightEyes.BrightEyes", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(open), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    
    return status;
}
