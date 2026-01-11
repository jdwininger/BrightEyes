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
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, archive_path, -1);
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
    const char *exts[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tiff", ".svg", ".webp", NULL };
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
            
            /* Save to cache */
            char *dir = g_path_get_dirname(cache_path);
            g_mkdir_with_parents(dir, 0700);
            g_free(dir);
            g_file_set_contents(cache_path, (const char *)data, (gssize)buf_len, NULL);

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
    /* For zip, we usually don't need filters on the output, just set format */
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
        
        /* Copy data */
        const void *buff;
        size_t size;
        int64_t offset;
        int r;
        while ((r = archive_read_data_block(in, &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(out, buff, size, offset) != ARCHIVE_OK) {
                g_warning("Failed to write data: %s", archive_error_string(out));
                break;
            }
        }
        if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
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

    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        /* We might want to ensure the entry name is clean or safe, but for conversion we typically keep it as is. */
        
        /* Copy header */
        /* Note: archive_write_header might fail if the format doesn't support some metadata from the source.
           We can clone the entry to be safe and strip unsupported fields, but libarchive usually does a best effort. */
           
        /* Explicitly unset the size if it is unknown? 
           Archive entries from RAR usually have size. */
        
        if (archive_write_header(out, entry) != ARCHIVE_OK) {
             g_warning("Failed to write header for %s: %s", archive_entry_pathname(entry), archive_error_string(out));
             /* Continue or fail? Let's try to continue or break if it's fatal context. */
             /* If we can't write header, we can't write data. */
             break;
        }
        
        /* Copy data */
        const void *buff;
        size_t size;
        int64_t offset;
        int r2;
        while ((r2 = archive_read_data_block(in, &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(out, buff, size, offset) != ARCHIVE_OK) {
                g_warning("Failed to write data block: %s", archive_error_string(out));
                break;
            }
        }
        if (r2 != ARCHIVE_EOF && r2 != ARCHIVE_OK) {
             g_warning("Error reading data block: %s", archive_error_string(in));
        }
    }

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
