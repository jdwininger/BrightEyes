#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "../src/archive.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <folder> <dest.cbz>\n", argv[0]);
        return 2;
    }
    const char *folder = argv[1];
    const char *dest = argv[2];
    GError *err = NULL;
    if (archive_create_cbz_from_folder(folder, dest, &err)) {
        printf("Success: %s -> %s\n", folder, dest);
        return 0;
    } else {
        if (err) {
            fprintf(stderr, "Error: %s\n", err->message);
            g_error_free(err);
        } else {
            fprintf(stderr, "Error: unknown\n");
        }
        return 1;
    }
}
