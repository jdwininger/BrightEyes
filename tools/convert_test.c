#include <stdio.h>
#include <glib.h>
#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source.cbr> <dest.cbz>\n", argv[0]);
        return 2;
    }
    const char *source_path = argv[1];
    const char *dest_path = argv[2];

#ifdef HAVE_LIBARCHIVE
    struct archive *in = archive_read_new();
    struct archive *out = archive_write_new();
    struct archive_entry *entry;
    int r;

    archive_read_support_format_all(in);
    archive_read_support_filter_all(in);
    archive_write_set_format_zip(out);

    if (archive_read_open_filename(in, source_path, 10240) != ARCHIVE_OK) {
        fprintf(stderr, "Failed to open source archive: %s\n", archive_error_string(in));
        return 1;
    }

    if (archive_write_open_filename(out, dest_path) != ARCHIVE_OK) {
        fprintf(stderr, "Failed to create destination archive: %s\n", archive_error_string(out));
        archive_read_free(in);
        archive_write_free(out);
        return 1;
    }

    int entries_total = 0;
    int entries_written = 0;

    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        int64_t entry_size = (int64_t)archive_entry_size(entry);
        entries_total++;
        g_message("found entry '%s' size=%lld", name ? name : "(null)", (long long)entry_size);

        if (name && name[strlen(name)-1] == '/') {
            g_message("skipping directory '%s'", name);
            archive_read_data_skip(in);
            continue;
        }

        if (archive_write_header(out, entry) != ARCHIVE_OK) {
            g_warning("Failed to write header for %s: %s", archive_entry_pathname(entry), archive_error_string(out));
            archive_read_data_skip(in);
            continue;
        }

        char tmpbuf[16384];
        ssize_t r2;
        ssize_t bytes_written_for_entry = 0;
        while ((r2 = archive_read_data(in, tmpbuf, sizeof(tmpbuf))) > 0) {
            ssize_t w = archive_write_data(out, tmpbuf, (size_t)r2);
            if (w < 0) {
                g_warning("Failed to write data for '%s': %s", name, archive_error_string(out));
                break;
            }
            bytes_written_for_entry += w;
        }
        if (r2 < 0) {
            g_warning("Error reading data for '%s': %s", name, archive_error_string(in));
        }

        g_message("wrote %zd bytes for '%s'", bytes_written_for_entry, name ? name : "(null)");
        entries_written++;
    }

    g_message("processed %d entries, wrote %d entries", entries_total, entries_written);

    if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
        fprintf(stderr, "Error reading source archive: %s\n", archive_error_string(in));
        archive_read_close(in);
        archive_read_free(in);
        archive_write_close(out);
        archive_write_free(out);
        unlink(dest_path);
        return 1;
    }

    archive_read_close(in);
    archive_read_free(in);
    archive_write_close(out);
    archive_write_free(out);

    return 0;
#else
    fprintf(stderr, "libarchive not available in build\n");
    return 1;
#endif
}
