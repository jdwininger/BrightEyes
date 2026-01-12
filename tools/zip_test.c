#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct archive *a = archive_write_new();
    archive_write_add_filter_none(a);
    archive_write_set_format_zip(a);
    if (archive_write_open_filename(a, "/tmp/ziptest.cbz") != ARCHIVE_OK) {
        fprintf(stderr, "open failed: %s\n", archive_error_string(a));
        return 1;
    }
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, "test.txt");
    const char *data = "hello world\n";
    archive_entry_set_size(entry, strlen(data));
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);
    ssize_t r = archive_write_data(a, data, strlen(data));
    printf("archive_write_data returned %zd, error: %s\n", r, archive_error_string(a));
    archive_write_finish_entry(a);
    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);
    return 0;
}
