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
    if (archive_convert_to_cbz(src, dst, &error)) {
        g_print("archive_convert_to_cbz succeeded\n");
        return 0;
    } else {
        g_print("archive_convert_to_cbz failed: %s\n", error ? error->message : "(no error)");
        if (error) g_error_free(error);
        return 1;
    }
}
