#define _GNU_SOURCE
#include "archive.h"
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

/* Internal helper: get cache path for an archive entry */
static char *
get_cache_path(const char *archive_path, const char *entry_name)
{
    /* Canonicalize the archive path to avoid different path variants
     * producing different cache directories for the same file. */
    char *canonical = g_canonicalize_filename(archive_path, NULL);
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, canonical ? canonical : archive_path, -1);
    g_free(canonical);
    char *cache_dir = g_build_filename(g_get_user_cache_dir(), "brighteyes", "archives", sum, NULL);
    g_free(sum);

    char *full_path = g_build_filename(cache_dir, entry_name, NULL);
    g_free(cache_dir);

    return full_path;
}

/* Internal helper: is filename an image we care about */
static gboolean
is_image_name(const char *name)
{
    if (!name) return FALSE;
    const char *exts[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tiff", ".svg", ".webp", ".avif", NULL };
    char *lower = g_ascii_strdown(name, -1);
    gboolean ok = FALSE;
    for (int i = 0; exts[i]; i++) {
        if (g_str_has_suffix(lower, exts[i])) {
            ok = TRUE;
            break;
        }
    }
    g_free(lower);
    return ok;
}

#ifdef HAVE_LIBARCHIVE

static int
compare_image_entries(gconstpointer a, gconstpointer b)
{
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    
#ifdef _GNU_SOURCE
    return strverscmp(sa, sb);
#else
    return g_ascii_strcasecmp(sa, sb);
#endif
}

gboolean
archive_list_image_entries(const char *archive_path, GPtrArray *out_entries, GError **error)
{
    struct archive *a = NULL;
    struct archive_entry *entry = NULL;

    a = archive_read_new();
    if (!a) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create archive reader");
        return FALSE;
    }

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open archive: %s", archive_error_string(a));
        archive_read_free(a);
        return FALSE;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (is_image_name(name)) {
            g_ptr_array_add(out_entries, g_strdup(name));
        }
        archive_read_data_skip(a);
    }

    archive_read_free(a);

    /* Sort entries naturally (e.g. 1.jpg, 2.jpg, 10.jpg) */
    g_ptr_array_sort(out_entries, compare_image_entries);

    return TRUE;
}

GBytes *
archive_read_entry_bytes(const char *archive_path, const char *entry_name, GError **error)
{
    /* Check Cache first */
    char *cache_path = get_cache_path(archive_path, entry_name);
    if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
        char *content = NULL;
        gsize len = 0;
        if (g_file_get_contents(cache_path, &content, &len, NULL)) {
            g_free(cache_path);
            return g_bytes_new_take(content, len);
        }
    }

    struct archive *a = NULL;
    struct archive_entry *entry = NULL;
    GBytes *res = NULL;

    a = archive_read_new();
    if (!a) {
        g_free(cache_path);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create archive reader");
        return NULL;
    }

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        g_free(cache_path);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open archive: %s", archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (g_strcmp0(name, entry_name) == 0) {
            /* Some entries may not report size; read into growable buffer */
            GByteArray *buf = g_byte_array_new();
            ssize_t r;
            char tmp[8192];
            while ((r = archive_read_data(a, tmp, sizeof(tmp))) > 0) {
                g_byte_array_append(buf, (const guint8*)tmp, r);
            }
            if (r < 0) {
                g_free(cache_path);
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Error reading entry: %s", archive_error_string(a));
                g_byte_array_free(buf, TRUE);
                archive_read_free(a);
                return NULL;
            }
            /* Transfer to GBytes */
            gsize buf_len = buf->len;
            guint8 *data = g_byte_array_free(buf, FALSE);
            res = g_bytes_new_take(data, buf_len);
            
            /* Save to cache atomically: write to a temp file then rename. */
            char *dir = g_path_get_dirname(cache_path);
            g_mkdir_with_parents(dir, 0700);
            g_free(dir);

            char *tmp_path = g_strdup_printf("%s.tmp-%u", cache_path, (unsigned)getpid());
            if (!g_file_set_contents(tmp_path, (const char *)data, (gssize)buf_len, NULL)) {
                /* If writing cache fails, ignore and continue (don't abort conversion).
                 * Ensure no stray temp file remains. */
                g_unlink(tmp_path);
            } else {
                /* Rename into place (atomic on same filesystem). */
                if (rename(tmp_path, cache_path) != 0) {
                    /* Failed to rename; remove temp file. */
                    g_unlink(tmp_path);
                }
            }
            g_free(tmp_path);

            archive_read_free(a);
            g_free(cache_path);
            return res;
        }
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    g_free(cache_path);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Entry '%s' not found in archive", entry_name);
    return NULL;
}

gboolean
archive_get_entry_size(const char *archive_path, const char *entry_name, guint64 *size, GError **error)
{
    struct archive *a = NULL;
    struct archive_entry *entry = NULL;

    a = archive_read_new();
    if (!a) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create archive reader");
        return FALSE;
    }

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open archive: %s", archive_error_string(a));
        archive_read_free(a);
        return FALSE;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (g_strcmp0(name, entry_name) == 0) {
            *size = (guint64)archive_entry_size(entry);
            archive_read_free(a);
            return TRUE;
        }
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Entry '%s' not found in archive", entry_name);
    return FALSE;
}

typedef struct {
    char *archive_path;
    char *entry_name;
} ArchiveTaskData;

static void
archive_read_task_data_free(ArchiveTaskData *data)
{
    g_free(data->archive_path);
    g_free(data->entry_name);
    g_free(data);
}

static void
archive_read_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    ArchiveTaskData *data = (ArchiveTaskData *)task_data;
    GError *error = NULL;
    GBytes *bytes = archive_read_entry_bytes(data->archive_path, data->entry_name, &error);
    
    if (bytes) {
        g_task_return_pointer(task, bytes, (GDestroyNotify)g_bytes_unref);
    } else {
        g_task_return_error(task, error);
    }
}

void
archive_read_entry_bytes_async(const char *archive_path, const char *entry_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    ArchiveTaskData *data = g_new0(ArchiveTaskData, 1);
    data->archive_path = g_strdup(archive_path);
    data->entry_name = g_strdup(entry_name);
    
    g_task_set_task_data(task, data, (GDestroyNotify)archive_read_task_data_free);
    g_task_run_in_thread(task, archive_read_thread);
    g_object_unref(task);
}

GBytes *
archive_read_entry_bytes_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}

gboolean
archive_delete_entry(const char *archive_path, const char *entry_name, GError **error)
{
    if (!g_str_has_suffix(archive_path, ".cbz") && !g_str_has_suffix(archive_path, ".zip") &&
        !g_str_has_suffix(archive_path, ".CBZ") && !g_str_has_suffix(archive_path, ".ZIP")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Modification only supported for Zip/CBZ archives");
        return FALSE;
    }

    struct archive *in = archive_read_new();
    struct archive *out = archive_write_new();
    struct archive_entry *entry;
    
    archive_read_support_format_zip(in);
    archive_read_support_filter_all(in);
    /* For zip, explicitly set no filter and use zip format */
    archive_write_add_filter_none(out);
    archive_write_set_format_zip(out);

    if (archive_read_open_filename(in, archive_path, 10240) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open archive: %s", archive_error_string(in));
        archive_read_free(in);
        archive_write_free(out);
        return FALSE;
    }

    char *tmp_path = g_strdup_printf("%s.tmp", archive_path);
    if (archive_write_open_filename(out, tmp_path) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create temp archive: %s", archive_error_string(out));
        archive_read_free(in);
        archive_write_free(out);
        g_free(tmp_path);
        return FALSE;
    }

    int ret;
    gboolean found = FALSE;
    while ((ret = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char *current_name = archive_entry_pathname(entry);
        
        if (g_strcmp0(current_name, entry_name) == 0) {
            found = TRUE;
            /* Skip this entry */
            archive_read_data_skip(in);
            continue;
        }

        /* Copy header */
        if (archive_write_header(out, entry) != ARCHIVE_OK) {
             g_warning("Failed to write header: %s", archive_error_string(out));
             /* Continue? Most likely fatal */
             break;
        }
        
        /* Copy data (use stream API for maximum compatibility with writers like zip) */
        char tmpbuf[8192];
        ssize_t rr;
        while ((rr = archive_read_data(in, tmpbuf, sizeof(tmpbuf))) > 0) {
            ssize_t wrote = archive_write_data(out, tmpbuf, rr);
            if (wrote < 0) {
                g_warning("Failed to write data: %s", archive_error_string(out));
                break;
            }
        }
        if (rr != ARCHIVE_EOF && rr < 0) {
            g_warning("Error reading data: %s", archive_error_string(in));
        }
    }

    archive_read_close(in);
    archive_read_free(in);
    archive_write_close(out);
    archive_write_free(out);

    if (found) {
        GFile *src = g_file_new_for_path(tmp_path);
        GFile *dest = g_file_new_for_path(archive_path);
        if (g_file_move(src, dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error)) {
             /* Success - clear cache */
             char *cache_path = get_cache_path(archive_path, entry_name);
             g_unlink(cache_path);
             g_free(cache_path);
        } else {
             /* Error set by g_file_move */
             found = FALSE; /* Treat as failure */
        }
        g_object_unref(src);
        g_object_unref(dest);
    } else {
        /* Not found, just delete temp */
        g_unlink(tmp_path);
    }
    
    g_free(tmp_path);
    return found;
}

gboolean
archive_convert_to_cbz(const char *source_path, const char *dest_path, GError **error)
{
    struct archive *in = archive_read_new();
    struct archive *out = archive_write_new();
    struct archive_entry *entry;
    int r;

    archive_read_support_format_all(in);
    archive_read_support_filter_all(in);
    
    archive_write_set_format_zip(out);

    if (archive_read_open_filename(in, source_path, 10240) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open source archive: %s", archive_error_string(in));
        archive_read_free(in);
        archive_write_free(out);
        return FALSE;
    }

    if (archive_write_open_filename(out, dest_path) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create destination archive: %s", archive_error_string(out));
        archive_read_free(in);
        archive_write_free(out);
        return FALSE;
    }

    int entries_total = 0;
    int entries_written = 0;

    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        int64_t entry_size = (int64_t)archive_entry_size(entry);
        entries_total++;
        g_info("archive_convert_to_cbz: found entry '%s' size=%lld", name ? name : "(null)", (long long)entry_size);

        /* Skip directory entries */
        if (name && name[strlen(name)-1] == '/') {
            g_info("archive_convert_to_cbz: skipping directory entry '%s'", name);
            archive_read_data_skip(in);
            continue;
        }

        /* Skip zero-length non-image entries (common in some RAR archives) */
        if (entry_size == 0 && !is_image_name(name)) {
            g_info("archive_convert_to_cbz: skipping empty non-image entry '%s'", name);
            archive_read_data_skip(in);
            continue;
        }

        /* Create a sanitized header for the destination (avoid carrying
         * over source-specific metadata that the ZIP writer may not support). */
        struct archive_entry *out_entry = archive_entry_new();
        archive_entry_set_pathname(out_entry, name);
        archive_entry_set_size(out_entry, entry_size);
        archive_entry_set_filetype(out_entry, AE_IFREG);
        archive_entry_set_perm(out_entry, 0644);
        if (archive_entry_mtime(entry) > 0) {
            archive_entry_set_mtime(out_entry, archive_entry_mtime(entry), 0);
        }

        if (archive_write_header(out, out_entry) != ARCHIVE_OK) {
             g_warning("Failed to write header for %s: %s", name, archive_error_string(out));
             archive_entry_free(out_entry);
             archive_read_data_skip(in);
             continue;
        }

        /* Read entire entry into memory (works reliably across formats).
         * First, check the cache; if it's a zero-length or mismatched file,
         * remove it so we re-read the original archive contents. */
        char *cache_path = get_cache_path(source_path, name);
        if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
            off_t csize = 0;
            struct stat st;
            if (stat(cache_path, &st) == 0) {
                csize = st.st_size;
            }
            if (csize != (off_t)entry_size) {
                /* Stale/incorrect cache - remove it so the archive reader will read fresh */
                g_unlink(cache_path);
            }
        }
        g_free(cache_path);

        GError *rerr = NULL;
        GBytes *gb = archive_read_entry_bytes(source_path, name, &rerr);
        if (!gb) {
            g_warning("Failed to read entry '%s': %s", name, rerr ? rerr->message : "unknown");
            if (rerr) g_clear_error(&rerr);
            archive_entry_free(out_entry);
            continue;
        }
        gsize gblen = 0;
        const guint8 *gdata = g_bytes_get_data(gb, &gblen);
        ssize_t wrote = archive_write_data(out, gdata, (size_t)gblen);
        if (wrote < 0) {
            g_warning("Failed to write data for '%s': %s", name, archive_error_string(out));
            g_bytes_unref(gb);
            archive_entry_free(out_entry);
            continue;
        }
        g_info("archive_convert_to_cbz: wrote %zd bytes for entry '%s'", (ssize_t)gblen, name ? name : "(null)");
        entries_written++;
        if (archive_write_finish_entry(out) != ARCHIVE_OK) {
            g_warning("Failed to finish entry '%s': %s", name, archive_error_string(out));
        }
        g_bytes_unref(gb);
        archive_entry_free(out_entry);
    }

    g_info("archive_convert_to_cbz: processed %d entries, wrote %d entries", entries_total, entries_written);

    if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
         g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Error reading source archive: %s", archive_error_string(in));
         archive_read_close(in);
         archive_read_free(in);
         archive_write_close(out);
         archive_write_free(out);
         /* Clean up partially written file */
         g_unlink(dest_path);
         return FALSE;
    }

    archive_read_close(in);
    archive_read_free(in);
    archive_write_close(out);
    archive_write_free(out);

    return TRUE;
}

gboolean
archive_create_cbz_from_folder(const char *folder_path, const char *dest_path, GError **error)
{
    if (!folder_path || !dest_path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments");
        return FALSE;
    }

    /* Gather image entries from folder */
    GDir *d = g_dir_open(folder_path, 0, NULL);
    if (!d) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open folder: %s", folder_path);
        return FALSE;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        /* Skip dotfiles */
        if (name[0] == '.') continue;
        if (is_image_name(name)) {
            char *full = g_build_filename(folder_path, name, NULL);
            g_ptr_array_add(entries, full);
        }
    }
    g_dir_close(d);

    if (entries->len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No image files found in folder");
        g_ptr_array_free(entries, TRUE);
        return FALSE;
    }

    /* Sort entries by filename naturally */
    g_ptr_array_sort(entries, compare_image_entries);

    /* Create temp destination on same directory as dest_path */
    char *tmp_path = g_strdup_printf("%s.tmp-%u", dest_path, (unsigned)getpid());

    struct archive *out = archive_write_new();
    if (!out) {
        g_free(tmp_path);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create archive writer");
        g_ptr_array_free(entries, TRUE);
        return FALSE;
    }

    archive_write_add_filter_none(out);
    archive_write_set_format_zip(out);

    if (archive_write_open_filename(out, tmp_path) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open temp archive: %s", archive_error_string(out));
        archive_write_free(out);
        g_free(tmp_path);
        g_ptr_array_free(entries, TRUE);
        return FALSE;
    }

    /* For each file, write a properly-sanitized header and stream data */
    for (guint i = 0; i < entries->len; i++) {
        char *full = g_ptr_array_index(entries, i);
        struct stat st;
        if (stat(full, &st) != 0) {
            g_warning("Skipping unreadable file: %s", full);
            continue;
        }

        const char *base = g_path_get_basename(full);
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, base);
        archive_entry_set_size(entry, (la_int64_t)st.st_size);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, st.st_mtime, 0);

        if (archive_write_header(out, entry) != ARCHIVE_OK) {
            g_warning("Failed to write header for %s: %s", base, archive_error_string(out));
            archive_entry_free(entry);
            continue;
        }

        FILE *f = fopen(full, "rb");
        if (!f) {
            g_warning("Failed to open %s for reading", full);
            archive_entry_free(entry);
            continue;
        }

        char buf[8192];
        size_t rr;
        while ((rr = fread(buf, 1, sizeof(buf), f)) > 0) {
            ssize_t wrote = archive_write_data(out, buf, (ssize_t)rr);
            if (wrote < 0) {
                g_warning("Failed to write data for %s: %s", base, archive_error_string(out));
                break;
            }
        }
        fclose(f);
        archive_entry_free(entry);
    }

    archive_write_close(out);
    archive_write_free(out);

    /* Rename into place atomically */
    if (rename(tmp_path, dest_path) != 0) {
        int eno = errno;
        g_set_error(error, G_IO_ERROR, g_file_error_from_errno(eno), "Failed to move temporary archive into place: %s", g_strerror(eno));
        g_unlink(tmp_path);
        g_free(tmp_path);
        g_ptr_array_free(entries, TRUE);
        return FALSE;
    }

    g_free(tmp_path);
    g_ptr_array_free(entries, TRUE);
    return TRUE;
}

#else /* HAVE_LIBARCHIVE */

gboolean
archive_list_image_entries(const char *archive_path, GPtrArray *out_entries, GError **error)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "libarchive support not compiled in");
    return FALSE;
}

GBytes *
archive_read_entry_bytes(const char *archive_path, const char *entry_name, GError **error)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "libarchive support not compiled in");
    return NULL;
}

gboolean
archive_get_entry_size(const char *archive_path, const char *entry_name, guint64 *size, GError **error)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "libarchive support not compiled in");
    return FALSE;
}

gboolean
archive_delete_entry(const char *archive_path, const char *entry_name, GError **error)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "libarchive support not compiled in");
    return FALSE;
}

gboolean
archive_convert_to_cbz(const char *source_path, const char *dest_path, GError **error)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "libarchive support not compiled in");
    return FALSE;
}

void
archive_read_entry_bytes_async(const char *archive_path, const char *entry_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "libarchive support not compiled in");
    g_object_unref(task);
}

GBytes *
archive_read_entry_bytes_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}

#endif /* HAVE_LIBARCHIVE */
