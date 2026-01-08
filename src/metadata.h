#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *metadata_sidebar_new(void);
void metadata_sidebar_update(GtkWidget *sidebar, const char *path);

G_END_DECLS
