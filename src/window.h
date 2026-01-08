#ifndef WINDOW_H
#define WINDOW_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_BRIGHT_EYES_WINDOW (bright_eyes_window_get_type())
G_DECLARE_FINAL_TYPE(BrightEyesWindow, bright_eyes_window, BRIGHT_EYES, WINDOW, AdwApplicationWindow)

BrightEyesWindow *bright_eyes_window_new(GtkApplication *app);
void bright_eyes_window_open_file(BrightEyesWindow *self, const char *path);

G_END_DECLS

#endif /* WINDOW_H */
