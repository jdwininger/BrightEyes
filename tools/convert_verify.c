#include <stdio.h>
#include <glib.h>
#include "../src/archive.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source.cbr> <dest.cbz>\n", argv[0]);
        return 2;
    }

    const char *src = argv[1];
    const char *dst = argv[2];
    GError *error = NULL;

    if (!archive_convert_to_cbz(src, dst, &error)) {
        fprintf(stderr, "archive_convert_to_cbz failed: %s\n", error ? error->message : "(no error)");
        if (error) g_error_free(error);
        return 1;
    }

    GError *err2 = NULL;
    GPtrArray *src_entries = g_ptr_array_new_with_free_func(g_free);
    if (!archive_list_image_entries(src, src_entries, &err2)) {
        fprintf(stderr, "Failed to list source entries: %s\n", err2 ? err2->message : "(no error)");
        if (err2) g_error_free(err2);
        g_ptr_array_free(src_entries, TRUE);
        return 1;
    }

    GPtrArray *dst_entries = g_ptr_array_new_with_free_func(g_free);
    if (!archive_list_image_entries(dst, dst_entries, &err2)) {
        fprintf(stderr, "Failed to list dest entries: %s\n", err2 ? err2->message : "(no error)");
        if (err2) g_error_free(err2);
        g_ptr_array_free(src_entries, TRUE);
        g_ptr_array_free(dst_entries, TRUE);
        return 1;
    }

    if (src_entries->len != dst_entries->len) {
        fprintf(stderr, "Entry count mismatch: src=%u dst=%u\n", (guint)src_entries->len, (guint)dst_entries->len);
        for (guint i = 0; i < src_entries->len; i++)
            g_print("src: %s\n", (char *)g_ptr_array_index(src_entries, i));
        for (guint i = 0; i < dst_entries->len; i++)
            g_print("dst: %s\n", (char *)g_ptr_array_index(dst_entries, i));
        g_ptr_array_free(src_entries, TRUE);
        g_ptr_array_free(dst_entries, TRUE);
        return 2;
    }

    g_print("Verification OK: %u entries copied\n", (guint)src_entries->len);

    g_ptr_array_free(src_entries, TRUE);
    g_ptr_array_free(dst_entries, TRUE);
    return 0;
}
