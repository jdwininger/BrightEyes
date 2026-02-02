#ifndef THUMBNAILS_H
#define THUMBNAILS_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "curator.h"

G_BEGIN_DECLS

#define TYPE_THUMBNAILS_BAR (thumbnails_bar_get_type())
G_DECLARE_FINAL_TYPE(ThumbnailsBar, thumbnails_bar, BRIGHTEYES, THUMBNAILS_BAR, GtkBox)

ThumbnailsBar *thumbnails_bar_new(Curator *curator);
void thumbnails_bar_refresh(ThumbnailsBar *self);

/* Capture a single video frame from `path` into a newly-allocated GdkPixbuf.
 * If `width` is > 0 the frame is scaled to that width (preserving aspect).
 * Returns TRUE on success and sets `out_pixbuf` (caller owns the ref). */
bool thumbnails_capture_video_frame(const char *path, int width, GdkPixbuf **out_pixbuf, GError **error);

G_END_DECLS

#endif /* THUMBNAILS_H */
