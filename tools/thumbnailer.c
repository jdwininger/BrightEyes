#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static gboolean is_image_filename(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return FALSE;
    ext++;
    if (!ext) return FALSE;
    if (g_ascii_strcasecmp(ext, "jpg") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "jpeg") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "png") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "gif") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "webp") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "bmp") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "tif") == 0) return TRUE;
    if (g_ascii_strcasecmp(ext, "tiff") == 0) return TRUE;
    return FALSE;
}

static char *maybe_uri_to_path(const char *uri) {
    if (g_str_has_prefix(uri, "file://")) {
        GError *err = NULL;
        char *path = g_filename_from_uri(uri, NULL, &err);
        if (!path) {
            g_clear_error(&err);
            return NULL;
        }
        return path;
    }
    return g_strdup(uri);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input-file-or-uri> <output-png> <size>\n", argv[0]);
        return 2;
    }

    char *inpath = maybe_uri_to_path(argv[1]);
    if (!inpath) {
        fprintf(stderr, "thumbnailer: unable to resolve input path: %s\n", argv[1]);
        return 2;
    }
    const char *outpath = argv[2];
    int size = atoi(argv[3]);
    if (size <= 0) size = 128;

    fprintf(stderr, "thumbnailer: in=%s out=%s size=%d cwd=%s\n", inpath, outpath, size, g_get_current_dir());

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, inpath, 10240) != ARCHIVE_OK) {
        fprintf(stderr, "thumbnailer: failed to open archive: %s\n", inpath);
        g_free(inpath);
        archive_read_free(a);
        return 1;
    }

    struct archive_entry *entry;
    int r;
    gchar *tmpdir = NULL;
    gchar *tmpfile = NULL;
    gboolean found = FALSE;

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!name) continue;
        if (!is_image_filename(name)) continue;

        tmpdir = g_strdup_printf("/tmp/brighteyes-thumb-XXXXXX");
        if (!mkdtemp(tmpdir)) {
            g_free(tmpdir);
            tmpdir = NULL;
            continue;
        }

        gchar *base = g_path_get_basename(name);
        tmpfile = g_build_filename(tmpdir, base, NULL);
        g_free(base);

        FILE *f = fopen(tmpfile, "wb");
        if (!f) {
            g_free(tmpfile);
            tmpfile = NULL;
            rmdir(tmpdir);
            g_free(tmpdir);
            tmpdir = NULL;
            continue;
        }

        const void *buff;
        size_t size_read;
        la_int64_t offset;
        while (1) {
            r = archive_read_data_block(a, &buff, &size_read, &offset);
            if (r == ARCHIVE_EOF) break;
            if (r != ARCHIVE_OK) break;
            if (fwrite(buff, 1, size_read, f) != size_read) break;
        }

        fclose(f);

        /* Try to load image */
        GError *gerr = NULL;
        GdkPixbuf *pix = gdk_pixbuf_new_from_file(tmpfile, &gerr);
        if (!pix) {
            if (gerr) {
                fprintf(stderr, "thumbnailer: gdk-pixbuf failed to load '%s': %s\n", tmpfile, gerr->message);
            } else {
                fprintf(stderr, "thumbnailer: gdk-pixbuf failed to load '%s' (unknown error)\n", tmpfile);
            }
            g_clear_error(&gerr);
            /* cleanup and try next image */
            unlink(tmpfile);
            g_free(tmpfile);
            tmpfile = NULL;
            rmdir(tmpdir);
            g_free(tmpdir);
            tmpdir = NULL;
            continue;
        }

        int width = gdk_pixbuf_get_width(pix);
        int height = gdk_pixbuf_get_height(pix);
        int target_w = size;
        int target_h = (height * size) / width;
        if (target_h <= 0) target_h = size;

        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pix, target_w, target_h, GDK_INTERP_BILINEAR);
        g_object_unref(pix);

        if (!gdk_pixbuf_save(scaled, outpath, "png", &gerr, NULL)) {
            fprintf(stderr, "thumbnailer: failed to save thumbnail '%s': %s\n", outpath, gerr ? gerr->message : "unknown");
            g_clear_error(&gerr);
            g_object_unref(scaled);
            unlink(tmpfile);
            g_free(tmpfile);
            rmdir(tmpdir);
            g_free(tmpdir);
            tmpfile = NULL;
            tmpdir = NULL;
            archive_read_free(a);
            g_free(inpath);
            return 1;
        }

        g_object_unref(scaled);

        found = TRUE;
        fprintf(stderr, "thumbnailer: wrote thumbnail '%s'\n", outpath);
        unlink(tmpfile);
        g_free(tmpfile);
        rmdir(tmpdir);
        g_free(tmpdir);
        tmpfile = NULL;
        tmpdir = NULL;
        break;
    }

    archive_read_free(a);
    g_free(inpath);

    return found ? 0 : 1;
}
