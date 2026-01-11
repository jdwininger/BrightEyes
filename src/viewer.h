#ifndef VIEWER_H
#define VIEWER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TYPE_VIEWER (viewer_get_type())
G_DECLARE_FINAL_TYPE(Viewer, viewer, VIEWER, TYPE_VIEWER, GtkBox)

#ifndef VIEWER
#define VIEWER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_VIEWER, Viewer))
#endif

Viewer *viewer_new(void);
void viewer_load_file(Viewer *self, const char *path);

void viewer_zoom_in(Viewer *self);
void viewer_zoom_out(Viewer *self);
void viewer_zoom_reset(Viewer *self);
void viewer_set_fit_to_window(Viewer *self, gboolean fit);
void viewer_set_fit_to_width(Viewer *self);
void viewer_rotate_cw(Viewer *self);
void viewer_rotate_ccw(Viewer *self);

/* Switch between dark and light background treatments for the viewing area. */
void viewer_set_dark_background(Viewer *self, gboolean dark);

/* Configure default fit mode when loading a new image. */
void viewer_set_default_fit(Viewer *self, gboolean fit);

guint viewer_get_zoom_level_percentage(Viewer *self);

void viewer_set_fit_to_width(Viewer *self);
gboolean viewer_is_fit_to_width(Viewer *self);

/* Returns TRUE if currently playing a video (GST_STATE_PLAYING). */
gboolean viewer_is_playing(Viewer *self);

/* Selection API */

/* Returns TRUE if there is an active selection rectangle. */
gboolean viewer_has_selection(Viewer *self);
void     viewer_set_selection_mode(Viewer *self, gboolean enabled);
gboolean viewer_get_selection_mode(Viewer *self);

/* Returns a newly allocated GdkPixbuf containing the selected region in image pixels, or NULL if no selection.
   Caller owns the returned pixbuf. */
GdkPixbuf *viewer_get_selection_pixbuf(Viewer *self);

/* Clear any existing selection. */
void viewer_clear_selection(Viewer *self);

G_END_DECLS

#endif // VIEWER_H
