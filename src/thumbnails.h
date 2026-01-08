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

G_END_DECLS

#endif /* THUMBNAILS_H */
