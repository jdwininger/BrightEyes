#include "curator.h"
#include <gio/gio.h>
#include <string.h>

/* Curator (model)
 *
 * Maintains the list of supported media files in a directory and the
 * current index. Provides navigation helpers and file operations.
 *
 * Sections: lifecycle (init/dispose), public API (load/get/set), helpers.
 */

struct _Curator {
    GObject parent_instance;
    GPtrArray *files; /* Array of full paths (strings) */
    int current_index;
    char *current_directory;
};

G_DEFINE_TYPE(Curator, curator, G_TYPE_OBJECT)

static void
curator_dispose(GObject *object)
{
    Curator *self = BRIGHTEYES_CURATOR(object);

    if (self->files) {
        g_ptr_array_unref(self->files);
        self->files = NULL;
    }

    G_OBJECT_CLASS(curator_parent_class)->dispose(object);
}

static void
curator_finalize(GObject *object)
{
    Curator *self = BRIGHTEYES_CURATOR(object);

    g_free(self->current_directory);

    G_OBJECT_CLASS(curator_parent_class)->finalize(object);
}

static void
curator_class_init(CuratorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = curator_dispose;
    object_class->finalize = curator_finalize;
}

static void
curator_init(Curator *self)
{
    self->files = g_ptr_array_new_with_free_func(g_free);
    self->current_index = -1;
}

Curator *
curator_new(void)
{
    return g_object_new(TYPE_CURATOR, NULL);
}

gboolean
curator_is_supported(const char *filename)
{
    const char *supported_exts[] = {
        ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tiff", ".svg", ".webp",
        ".mp4", ".mkv", ".webm", ".avi", ".mov", ".heic", ".avif",
        NULL
    };
    
    char *lower = g_ascii_strdown(filename, -1);
    gboolean supported = FALSE;
    
    for (int i = 0; supported_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower, supported_exts[i])) {
            supported = TRUE;
            break;
        }
    }
    g_free(lower);
    return supported;
}

static int
compare_strings(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}

void
curator_load_directory(Curator *self, const char *path)
{
    g_free(self->current_directory);
    self->current_directory = g_strdup(path);
    
    g_ptr_array_set_size(self->files, 0);
    self->current_index = -1;

    GFile *dir = g_file_new_for_path(path);
    GFileEnumerator *enumerator = g_file_enumerate_children(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NONE,
        NULL, NULL);

    if (enumerator) {
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(enumerator, NULL, NULL))) {
            const char *name = g_file_info_get_name(info);
            if (!g_str_has_prefix(name, ".") && curator_is_supported(name)) {
                char *full_path = g_build_filename(path, name, NULL);
                g_ptr_array_add(self->files, full_path);
            }
            g_object_unref(info);
        }
        g_object_unref(enumerator);
    }
    g_object_unref(dir);

    g_ptr_array_sort(self->files, compare_strings);
}

void
curator_set_current_file(Curator *self, const char *filepath)
{
    if (!filepath) return;

    /* If file is not in current directory, load its directory first */
    char *dirname = g_path_get_dirname(filepath);
    if (!self->current_directory || g_strcmp0(dirname, self->current_directory) != 0) {
        curator_load_directory(self, dirname);
    }
    g_free(dirname);

    /* Find index */
    for (guint i = 0; i < self->files->len; i++) {
        const char *f = g_ptr_array_index(self->files, i);
        if (g_strcmp0(f, filepath) == 0) {
            self->current_index = i;
            return;
        }
    }
    
    /* If not found (maybe separate casing or full path issue), standardizing needed? 
       For now, assume load_directory found it if it exists. 
       If not found, maybe add it? */
       
    /* Should we try to match just filename if full path compare failed? */
    char *basename = g_path_get_basename(filepath);
    for (guint i = 0; i < self->files->len; i++) {
        char *f_base = g_path_get_basename(g_ptr_array_index(self->files, i));
        if (g_strcmp0(f_base, basename) == 0) {
             self->current_index = i;
             g_free(f_base);
             g_free(basename);
             return;
        }
        g_free(f_base);
    }
    g_free(basename);
}

const char *
curator_get_current(Curator *self)
{
    if (self->files->len == 0) return NULL;
    if (self->current_index < 0 || self->current_index >= (int)self->files->len) {
        if (self->files->len > 0) {
            self->current_index = 0;
            return g_ptr_array_index(self->files, 0);
        }
        return NULL;
    }
    return g_ptr_array_index(self->files, self->current_index);
}

const char *
curator_get_next(Curator *self)
{
    if (self->files->len == 0) return NULL;
    self->current_index++;
    if (self->current_index >= (int)self->files->len) {
        self->current_index = 0; /* Loop */
    }
    return g_ptr_array_index(self->files, self->current_index);
}

const char *
curator_get_prev(Curator *self)
{
    if (self->files->len == 0) return NULL;
    self->current_index--;
    if (self->current_index < 0) {
        self->current_index = self->files->len - 1; /* Loop */
    }
    return g_ptr_array_index(self->files, self->current_index);
}

GPtrArray *
curator_get_files(Curator *self)
{
    return self->files;
}

gboolean
curator_trash_current(Curator *self, GError **error)
{
    const char *current = curator_get_current(self);
    if (!current) return FALSE;

    GFile *file = g_file_new_for_path(current);
    gboolean ok = g_file_trash(file, NULL, error);
    g_object_unref(file);
    if (!ok) return FALSE;

    if (self->current_index >= 0 && self->current_index < (int)self->files->len) {
        g_ptr_array_remove_index(self->files, self->current_index);
    }

    if (self->files->len == 0) {
        self->current_index = -1;
        return TRUE;
    }

    if (self->current_index >= (int)self->files->len)
        self->current_index = (int)self->files->len - 1;

    return TRUE;
}
