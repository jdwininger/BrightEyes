#include "thumbnails.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gst/gst.h>
#include <sys/stat.h>

/* Thumbnails (UI)
 *
 * Implements the thumbnail list UI used by the main window.
 * - ThumbnailItem: lightweight GObject holding a file path and paintable.
 * - ThumbnailsBar: container that manages the grid/list and async loading.
 *
 * Sections: ThumbnailItem, helpers, lifecycle (init/dispose), and bar API.
 */

/* --- ThumbnailItem Object --- */

#define TYPE_THUMBNAIL_ITEM (thumbnail_item_get_type())
G_DECLARE_FINAL_TYPE (ThumbnailItem, thumbnail_item, BRIGHTEYES, THUMBNAIL_ITEM, GObject)

struct _ThumbnailItem {
    GObject parent;
    char *path;
    GdkPaintable *paintable;
    gboolean loading;
    guint load_timeout_id; /* non-zero when a delayed load is scheduled */
};

enum {
    PROP_0,
    PROP_PATH,
    PROP_PAINTABLE,
    N_PROPS
};

static GParamSpec *item_props[N_PROPS] = { NULL, };

G_DEFINE_TYPE (ThumbnailItem, thumbnail_item, G_TYPE_OBJECT)

static void
thumbnail_item_finalize(GObject *object)
{
    ThumbnailItem *self = BRIGHTEYES_THUMBNAIL_ITEM(object);
    g_free(self->path);
    g_clear_object(&self->paintable);
    G_OBJECT_CLASS(thumbnail_item_parent_class)->finalize(object);
}

static void
thumbnail_item_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    ThumbnailItem *self = BRIGHTEYES_THUMBNAIL_ITEM(object);
    switch (prop_id) {
        case PROP_PATH: g_value_set_string(value, self->path); break;
        case PROP_PAINTABLE: g_value_set_object(value, self->paintable); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
    }
}

static void
thumbnail_item_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    ThumbnailItem *self = BRIGHTEYES_THUMBNAIL_ITEM(object);
    switch (prop_id) {
        case PROP_PATH: self->path = g_value_dup_string(value); break;
        case PROP_PAINTABLE: 
            g_clear_object(&self->paintable);
            self->paintable = g_value_dup_object(value);
            break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
    }
}

static void
thumbnail_item_class_init(ThumbnailItemClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = thumbnail_item_finalize;
    obj_class->get_property = thumbnail_item_get_property;
    obj_class->set_property = thumbnail_item_set_property;

    item_props[PROP_PATH] = g_param_spec_string("path", "Path", "File Path", NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    item_props[PROP_PAINTABLE] = g_param_spec_object("paintable", "Paintable", "Thumbnail Image", GDK_TYPE_PAINTABLE, G_PARAM_READWRITE);
    
    g_object_class_install_properties(obj_class, N_PROPS, item_props);
}

static void
thumbnail_item_init(ThumbnailItem *self) {
    self->loading = FALSE;
    self->load_timeout_id = 0;
}

static ThumbnailItem *
thumbnail_item_new(const char *path) {
    return g_object_new(TYPE_THUMBNAIL_ITEM, "path", path, NULL);
}

/* Video thumbnail worker pool - limits concurrency for expensive work */
static GThreadPool *video_thread_pool = NULL;
static void video_pool_worker(gpointer data, gpointer user_data);
static void thumbnail_item_ensure_loaded(ThumbnailItem *self);
static gboolean thumbnail_load_timeout_cb(gpointer user_data);

/* LRU in-memory cache for paintables (session only). Key is path + mtime+size. */
static GHashTable *thumbnail_cache_map = NULL; /* key (char*) -> GdkPaintable* */
static GQueue *thumbnail_cache_lru = NULL;     /* queue of key pointers in LRU order */
static guint thumbnail_cache_max_entries = 256;
static void lru_cache_init(void);
static gchar *make_cache_key(const char *path);
static GdkPaintable *lru_cache_get(const char *key);
static void lru_cache_put(const char *key, GdkPaintable *paintable);
static void lru_cache_destroy(void);

/* --- Instrumentation counters (optional; enabled by env) --- */
static guint instr_cache_hits = 0;
static guint instr_cache_misses = 0;
static guint instr_ignored_notifies = 0;
static guint instr_video_tasks_started = 0;
static guint instr_video_tasks_completed = 0;
static gboolean instrumentation_enabled = FALSE;
static void instrumentation_init(void);
static void thumbnails_print_instrumentation(void);

/* Forward declare helper */
static gboolean is_video(const char *path);

static GdkTexture *
texture_from_pixbuf(GdkPixbuf *pixbuf) {
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    
    GBytes *bytes = g_bytes_new_with_free_func(gdk_pixbuf_get_pixels(pixbuf),
                                               (gsize)stride * height,
                                               (GDestroyNotify)g_object_unref,
                                               g_object_ref(pixbuf));
                                               
    GdkTexture *texture = gdk_memory_texture_new(width, height,
                                                 has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                 bytes, stride);
    g_bytes_unref(bytes);
    return texture;
}

static void
lru_cache_init(void)
{
    if (thumbnail_cache_map) return;
    thumbnail_cache_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);
    thumbnail_cache_lru = g_queue_new();

    /* Initialize instrumentation lazily whenever cache is used */
    instrumentation_init();
}

static void
instrumentation_init(void)
{
    if (instrumentation_enabled) return;
    if (g_getenv("BRIGHTEYES_THUMBNAILS_DEBUG") != NULL) {
        instrumentation_enabled = TRUE;
        g_debug("THUMBS-INSTR: instrumentation enabled");
    }
}

static void
thumbnails_print_instrumentation(void)
{
    if (!instrumentation_enabled) return;
    g_info("THUMBS-INSTR: cache_hits=%u cache_misses=%u ignored_notifies=%u video_started=%u video_completed=%u",
           instr_cache_hits, instr_cache_misses, instr_ignored_notifies, instr_video_tasks_started, instr_video_tasks_completed);
}

static gchar *
make_cache_key(const char *path)
{
    if (!path) return NULL;
    struct stat st;
    if (stat(path, &st) != 0) return g_strdup(path); /* fallback to path only */
    return g_strdup_printf("%s:%llu:%llu", path, (unsigned long long)st.st_mtime, (unsigned long long)st.st_size);
}

static GdkPaintable *
lru_cache_get(const char *key)
{
    if (!key) return NULL;
    lru_cache_init();
    gpointer actual_key = NULL;
    gpointer val = NULL;
    if (g_hash_table_lookup_extended(thumbnail_cache_map, key, &actual_key, &val)) {
        /* Move key to tail (most-recent) */
        g_queue_remove(thumbnail_cache_lru, actual_key);
        g_queue_push_tail(thumbnail_cache_lru, actual_key);
        g_object_ref(val);
        if (instrumentation_enabled) {
            instr_cache_hits++;
            g_debug("THUMBS-INSTR: cache hit -> %s (hits=%u)", key, instr_cache_hits);
        }
        return GDK_PAINTABLE(val);
    }
    return NULL;
}

static void
lru_cache_put(const char *key, GdkPaintable *paintable)
{
    if (!key || !paintable) return;
    lru_cache_init();

    gpointer actual_key = NULL;
    gpointer val = NULL;
    if (g_hash_table_lookup_extended(thumbnail_cache_map, key, &actual_key, &val)) {
        /* Replace value (hash table will unref old one via destroy notify) */
        g_hash_table_insert(thumbnail_cache_map, actual_key, g_object_ref(paintable));
        /* Move key to tail */
        g_queue_remove(thumbnail_cache_lru, actual_key);
        g_queue_push_tail(thumbnail_cache_lru, actual_key);
    } else {
        char *kdup = g_strdup(key);
        g_hash_table_insert(thumbnail_cache_map, kdup, g_object_ref(paintable));
        g_queue_push_tail(thumbnail_cache_lru, kdup);
    }

    /* Evict if needed */
    while (g_hash_table_size(thumbnail_cache_map) > thumbnail_cache_max_entries) {
        char *old_key = g_queue_pop_head(thumbnail_cache_lru);
        if (old_key) {
            g_hash_table_remove(thumbnail_cache_map, old_key);
            /* g_hash_table_remove will free the key and unref the value */
        }
    }
}

static void
lru_cache_destroy(void)
{
    if (thumbnail_cache_map) {
        g_hash_table_destroy(thumbnail_cache_map);
        thumbnail_cache_map = NULL;
    }
    if (thumbnail_cache_lru) {
        g_queue_free(thumbnail_cache_lru);
        thumbnail_cache_lru = NULL;
    }
}

static void
on_pixbuf_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ThumbnailItem *self = BRIGHTEYES_THUMBNAIL_ITEM(user_data);
    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_finish(res, &err);
    
    if (pixbuf) {
        GdkTexture *texture = texture_from_pixbuf(pixbuf);
        /* Cache by path+mtime so repeated opens avoid re-decoding */
        gchar *key = make_cache_key(self->path);
        if (key) {
            lru_cache_put(key, GDK_PAINTABLE(texture));
            g_free(key);
        }
        g_object_set(self, "paintable", texture, NULL);
        g_object_unref(texture);
        g_object_unref(pixbuf);
    } else {
        /* Failed or cancelled */
        g_clear_error(&err);
    }
    g_object_unref(self); /* Dec ref from load start */
}

static void
create_video_thumbnail_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    char *path = (char *)task_data;
    GError *err = NULL;

    if (!gst_is_initialized()) gst_init(NULL, NULL);

    if (!path || path[0] == '\0') {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid path");
        return;
    }

    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    /* Request 128px width, variable height to preserve aspect ratio. */
    gchar *pipeline_cmd = g_strdup_printf(
        "uridecodebin uri=\"%s\" ! videoconvert ! videoscale ! video/x-raw,width=128,pixel-aspect-ratio=1/1 ! gdkpixbufsink name=sink",
        uri
    );
    g_free(uri);

    GstElement *pipeline = gst_parse_launch(pipeline_cmd, &err);
    g_free(pipeline_cmd);

    if (!pipeline) {
        g_task_return_error(task, err);
        return;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    GstStateChangeReturn ret = gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND);

    GdkPixbuf *pixbuf = NULL;
    if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL) {
        g_object_get(sink, "last-pixbuf", &pixbuf, NULL);
    } 

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (sink) gst_object_unref(sink);

    if (!pixbuf) {
         g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to capture video frame");
    } else {
        g_task_return_pointer(task, pixbuf, g_object_unref);
    }
}

/* Worker wrapper to execute GTask-based video thumbnail jobs inside a bounded pool. */
static void
video_pool_worker(gpointer data, gpointer user_data)
{
    GTask *task = G_TASK(data);
    /* Reuse the same thread function implementation to perform work */
    create_video_thumbnail_thread(task,
                                 g_task_get_source_object(task),
                                 g_task_get_task_data(task),
                                 g_task_get_cancellable(task));
    /* Worker owned reference - drop it now that the task has returned */
    g_object_unref(task);
}

static void
on_video_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ThumbnailItem *self = BRIGHTEYES_THUMBNAIL_ITEM(source);
    GError *err = NULL;
    GdkPixbuf *pixbuf = g_task_propagate_pointer(G_TASK(res), &err);

    if (pixbuf) {
        GdkTexture *texture = texture_from_pixbuf(pixbuf);
        gchar *key = make_cache_key(self->path);
        if (key) {
            lru_cache_put(key, GDK_PAINTABLE(texture));
            g_free(key);
        }
        g_object_set(self, "paintable", texture, NULL);
        g_object_unref(texture);
        g_object_unref(pixbuf);
    } else {
        g_clear_error(&err);
    }

    if (instrumentation_enabled) {
        instr_video_tasks_completed++;
        g_debug("THUMBS-INSTR: video task completed for %s (completed=%u)", self->path ? self->path : "(null)", instr_video_tasks_completed);
    }

    g_object_unref(self);
}

/* Delayed load callback used to debounce loads while the user is scrolling. */
static gboolean
thumbnail_load_timeout_cb(gpointer user_data)
{
    ThumbnailItem *self = BRIGHTEYES_THUMBNAIL_ITEM(user_data);
    self->load_timeout_id = 0;
    thumbnail_item_ensure_loaded(self);
    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static gboolean
is_video(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return FALSE;
    return (g_ascii_strcasecmp(ext, ".mp4") == 0 || g_ascii_strcasecmp(ext, ".mkv") == 0 ||
            g_ascii_strcasecmp(ext, ".webm") == 0 || g_ascii_strcasecmp(ext, ".avi") == 0);
}

static void
thumbnail_item_ensure_loaded(ThumbnailItem *self)
{
    /* Avoid starting work if already loaded or loading */
    if (self->paintable || self->loading) return;

    /* Check LRU cache first */
    gchar *key = make_cache_key(self->path);
    if (key) {
        GdkPaintable *cached = lru_cache_get(key);
        if (cached) {
            g_object_set(self, "paintable", cached, NULL);
            g_object_unref(cached);
            g_free(key);
            return;
        }
        /* Cache miss */
        if (instrumentation_enabled) {
            instr_cache_misses++;
            g_debug("THUMBS-INSTR: cache miss -> %s (misses=%u)", key, instr_cache_misses);
        }
        g_free(key);
    }

    self->loading = TRUE;
    /* If a delayed load was scheduled, cancel it now */
    if (self->load_timeout_id != 0) {
        g_source_remove(self->load_timeout_id);
        self->load_timeout_id = 0;
    }

    g_object_ref(self); /* Keep alive during async */

    if (is_video(self->path)) {
        if (instrumentation_enabled) {
            instr_video_tasks_started++;
            g_debug("THUMBS-INSTR: video task start for %s (started=%u)", self->path, instr_video_tasks_started);
        }

        GTask *task = g_task_new(self, NULL, on_video_loaded, NULL);
        g_task_set_task_data(task, g_strdup(self->path), g_free);
        /* Use a bounded pool for video processing (heavy) to avoid creating too many threads */
        if (!video_thread_pool) {
            video_thread_pool = g_thread_pool_new(video_pool_worker, NULL, 2, FALSE, NULL);
        }
        /* Ensure the task stays alive until the worker runs and releases it */
        g_object_ref(task);
        g_thread_pool_push(video_thread_pool, task, NULL);
        /* Drop caller's temporary ref; worker holds the ref until completion */
        g_object_unref(task);
        return;
    }
    
    GFile *file = g_file_new_for_path(self->path);
    /* Note: Sync read of file handle, but decoding is async. Acceptable for now. */
    GFileInputStream *stream = g_file_read(file, NULL, NULL); 
    
    if (stream) {
        gdk_pixbuf_new_from_stream_at_scale_async(G_INPUT_STREAM(stream), 128, 128, TRUE, NULL, on_pixbuf_loaded, self);
        g_object_unref(stream);
    } else {
        /* Failed to open */
        self->loading = FALSE;
        g_object_unref(self);
    }
    g_object_unref(file);
}


/* --- ThumbnailsBar --- */

struct _ThumbnailsBar {
    GtkBox parent_instance;
    Curator *curator;
    GtkScrolledWindow *scroller;
    GtkGridView *grid_view;
    GListStore *store;
    GtkSingleSelection *selection_model;
};

enum {
    SIGNAL_FILE_ACTIVATED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(ThumbnailsBar, thumbnails_bar, GTK_TYPE_BOX)

static void
setup_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_append(GTK_BOX(box), overlay);
    
    GtkWidget *picture = gtk_picture_new();
    gtk_widget_set_size_request(picture, 128, 128);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);
    
    GtkWidget *icon = gtk_image_new();
    gtk_widget_set_halign(icon, GTK_ALIGN_END);
    gtk_widget_set_valign(icon, GTK_ALIGN_END);
    gtk_widget_set_margin_end(icon, 4);
    gtk_widget_set_margin_bottom(icon, 4);
    gtk_widget_set_size_request(icon, 24, 24);
    gtk_widget_add_css_class(icon, "type-overlay-icon");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), icon);
    
    /* Label removed as per request */
    
    gtk_list_item_set_child(list_item, box);
}

static void
on_item_paintable_notify(ThumbnailItem *item, GParamSpec *pspec, gpointer user_data)
{
    GtkPicture *picture = GTK_PICTURE(user_data);
    /* Only update the picture if it is still bound to this item (guard against
       recycled widgets being reused for different items) */
    ThumbnailItem *bound = g_object_get_data(G_OBJECT(picture), "thumbnail-bound-item");
    if (bound != item) {
        if (instrumentation_enabled) {
            instr_ignored_notifies++;
            g_debug("THUMBS-INSTR: ignored notify for item %s (ignored=%u)", item->path ? item->path : "(null)", instr_ignored_notifies);
        }
        return;
    }

    gtk_picture_set_paintable(picture, item->paintable);
}

static void
bind_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    GtkWidget *box = gtk_list_item_get_child(list_item);
    ThumbnailItem *item = gtk_list_item_get_item(list_item);
    
    GtkWidget *overlay = gtk_widget_get_first_child(box);
    GtkWidget *picture = gtk_overlay_get_child(GTK_OVERLAY(overlay));
    
    /* Find the icon (overlay child) */
    GtkWidget *child = gtk_widget_get_first_child(overlay);
    while (child == picture) {
        child = gtk_widget_get_next_sibling(child);
    }
    GtkWidget *icon = child;
    
    /* Tooltip remains useful */
    gtk_widget_set_tooltip_text(box, item->path);
    
    /* Set Image */
    gtk_picture_set_paintable(GTK_PICTURE(picture), item->paintable);
    
    /* Set Icon Type */
    if (is_video(item->path)) {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "video-x-generic-symbolic");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "image-x-generic-symbolic");
    }

    /* Disconnect previously-bound item for this picture (if any) to avoid
       stale notify handlers updating this recycled widget later. */
    ThumbnailItem *prev = g_object_get_data(G_OBJECT(picture), "thumbnail-bound-item");
    if (prev && prev != item) {
        g_signal_handlers_disconnect_by_func(prev, on_item_paintable_notify, picture);
    }

    /* Bind current item -> picture association and hook notify handler */
    g_object_set_data(G_OBJECT(picture), "thumbnail-bound-item", item);
    if (prev != item) {
        g_signal_connect(item, "notify::paintable", G_CALLBACK(on_item_paintable_notify), picture);
    }

    /* Trigger load with a small debounce to avoid bursts during fast scrolling.
       Use a longer delay for videos so we only generate video thumbnails after
       the user stops scrolling. */
    if (!item->paintable && !item->loading) {
        if (item->load_timeout_id == 0) {
            guint delay = is_video(item->path) ? 500 : 80;
            g_object_ref(item);
            item->load_timeout_id = g_timeout_add(delay, thumbnail_load_timeout_cb, item);
        }
    }
}

static void
unbind_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    /* Find picture widget for this list item */
    GtkWidget *box = gtk_list_item_get_child(list_item);
    if (box) {
        GtkWidget *overlay = gtk_widget_get_first_child(box);
        if (overlay) {
            GtkWidget *picture = gtk_overlay_get_child(GTK_OVERLAY(overlay));
            if (picture) {
                /* Clear bound-item marker so any in-flight notify handlers will ignore */
                g_object_set_data(G_OBJECT(picture), "thumbnail-bound-item", NULL);

                /* Also disconnect any leftover notify handler for this picture on the item */
                ThumbnailItem *item = gtk_list_item_get_item(list_item);
                if (item) {
                    g_signal_handlers_disconnect_by_func(item, on_item_paintable_notify, picture);
                }
            }
        }
    }

    /* Cancel any pending delayed load */
    ThumbnailItem *item = gtk_list_item_get_item(list_item);
    if (item != NULL && item->load_timeout_id != 0) {
        g_source_remove(item->load_timeout_id);
        item->load_timeout_id = 0;
    }

    (void)factory;
    (void)user_data;
}

static void
on_selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer user_data)
{
    ThumbnailsBar *self = BRIGHTEYES_THUMBNAILS_BAR(user_data);
    GtkSingleSelection *selection = GTK_SINGLE_SELECTION(model);
    
    ThumbnailItem *item = gtk_single_selection_get_selected_item(selection);
    if (item) {
        g_signal_emit(self, signals[SIGNAL_FILE_ACTIVATED], 0, item->path);
    }
}

static void
thumbnails_bar_init(ThumbnailsBar *self)
{
    static gboolean css_loaded = FALSE;
    if (!css_loaded) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider,
            ".type-overlay-icon { \n"
            /* "  color: white; \n"  -- Removed to allow dark icon in light mode */
            "  text-shadow: 0 1px 1px rgba(0,0,0,0.8); \n"
            "  -gtk-icon-shadow: 0 1px 1px rgba(0,0,0,0.8); \n" 
            "}"
        );
        GdkDisplay *display = gdk_display_get_default();
        if (display) {
            gtk_style_context_add_provider_for_display(display, 
                GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            css_loaded = TRUE;
        }
        g_object_unref(provider);
    }

    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    
    /* Header */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(header_box, 12);
    gtk_widget_set_margin_bottom(header_box, 12);
    gtk_widget_set_margin_start(header_box, 12);
    gtk_widget_set_margin_end(header_box, 12);
    
    GtkWidget *title = gtk_label_new("Files");
    gtk_widget_add_css_class(title, "title-4");
    gtk_box_append(GTK_BOX(header_box), title);
    gtk_box_append(GTK_BOX(self), header_box);
    gtk_box_append(GTK_BOX(self), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    
    /* Scroller */
    self->scroller = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_widget_set_vexpand(GTK_WIDGET(self->scroller), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->scroller), TRUE);
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->scroller));
    
    /* Grid View Setup */
    self->store = g_list_store_new(TYPE_THUMBNAIL_ITEM);
    self->selection_model = gtk_single_selection_new(G_LIST_MODEL(self->store));
    g_signal_connect(self->selection_model, "selection-changed", G_CALLBACK(on_selection_changed), self);
    
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_list_item), self);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_list_item), self);
    g_signal_connect(factory, "unbind", G_CALLBACK(unbind_list_item), self);
    
    self->grid_view = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(self->selection_model), factory));
    gtk_grid_view_set_max_columns(self->grid_view, 1);
    gtk_grid_view_set_min_columns(self->grid_view, 1);
    
    gtk_scrolled_window_set_child(self->scroller, GTK_WIDGET(self->grid_view));
}

static void
thumbnails_bar_dispose(GObject *object)
{
    ThumbnailsBar *self = BRIGHTEYES_THUMBNAILS_BAR(object);

    /* Don't clear the store here - the selection_model holds a reference to it,
       and the grid_view holds a reference to selection_model. GTK will dispose
       the grid_view as part of the widget hierarchy teardown, which will then
       release the selection_model, which will release the store.
       
       Clearing the store prematurely causes crashes when GTK tries to access
       the model during its own cleanup. */
    
    self->store = NULL;
    self->selection_model = NULL;
    self->grid_view = NULL;

    /* Destroy in-memory thumbnail cache on dispose to free memory */
    lru_cache_destroy();

    /* If instrumentation was enabled, print a summary to the logs */
    thumbnails_print_instrumentation();

    g_clear_object(&self->curator);
    G_OBJECT_CLASS(thumbnails_bar_parent_class)->dispose(object);
}

static void
thumbnails_bar_class_init(ThumbnailsBarClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = thumbnails_bar_dispose;
    
    signals[SIGNAL_FILE_ACTIVATED] = g_signal_new("file-activated",
                                                  G_TYPE_FROM_CLASS(klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL, NULL,
                                                  NULL,
                                                  G_TYPE_NONE,
                                                  1,
                                                  G_TYPE_STRING);
}

ThumbnailsBar *
thumbnails_bar_new(Curator *curator)
{
    ThumbnailsBar *self = g_object_new(TYPE_THUMBNAILS_BAR, NULL);
    self->curator = curator ? g_object_ref(curator) : NULL;
    return self;
}

void
thumbnails_bar_refresh(ThumbnailsBar *self)
{
    if (!self->curator) return;
    
    g_list_store_remove_all(self->store);
    
    GPtrArray *files = curator_get_files(self->curator);
    if (!files) return;
    
    for (guint i = 0; i < files->len; i++) {
        const char *path = g_ptr_array_index(files, i);
        ThumbnailItem *item = thumbnail_item_new(path);
        g_list_store_append(self->store, item);
        g_object_unref(item); /* Store takes ownership */
    }
}
