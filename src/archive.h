#ifndef BRIGHTEYES_ARCHIVE_H
#define BRIGHTEYES_ARCHIVE_H

#include <glib.h>
#include <gio/gio.h>

/* List image entries inside an archive. Returns TRUE on success and fills
 * out_entries with newly allocated strings (use g_ptr_array_unref to free).
 */

gboolean archive_list_image_entries(const char *archive_path, GPtrArray *out_entries, GError **error);

/* Read a single entry from the archive into a new GBytes. Caller owns the
 * returned GBytes and must unref it. */
GBytes *archive_read_entry_bytes(const char *archive_path, const char *entry_name, GError **error);

/* Asynchronous version of archive_read_entry_bytes */
void archive_read_entry_bytes_async(const char *archive_path, const char *entry_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
GBytes *archive_read_entry_bytes_finish(GAsyncResult *res, GError **error);

/* Get size of entry without extracting (if available). */
gboolean archive_get_entry_size(const char *archive_path, const char *entry_name, guint64 *size, GError **error);

/* Delete a single entry from the archive (Archives must be Zip/CBZ).
 * This performs a streaming rewrite of the archive. */
gboolean archive_delete_entry(const char *archive_path, const char *entry_name, GError **error);

/* Convert any supported archive format (like CBR/RAR) to a CBZ (Zip) archive. */
gboolean archive_convert_to_cbz(const char *source_path, const char *dest_path, GError **error);

#endif /* BRIGHTEYES_ARCHIVE_H */
