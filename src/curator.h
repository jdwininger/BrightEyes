#ifndef CURATOR_H
#define CURATOR_H

#include <glib-object.h>

G_BEGIN_DECLS

#define TYPE_CURATOR (curator_get_type())
G_DECLARE_FINAL_TYPE(Curator, curator, BRIGHTEYES, CURATOR, GObject)

Curator *curator_new(void);

void curator_load_directory(Curator *self, const char *path);
void curator_set_current_file(Curator *self, const char *filepath);

const char *curator_get_current(Curator *self);
const char *curator_get_next(Curator *self);
const char *curator_get_prev(Curator *self);
GPtrArray *curator_get_files(Curator *self);

/* Move current file to trash; returns TRUE on success and updates the current index to a valid item if any remain. */
gboolean curator_trash_current(Curator *self, GError **error);

/* Utility to check if file is supported */
gboolean curator_is_supported(const char *filename);

G_END_DECLS

#endif /* CURATOR_H */
