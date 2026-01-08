/* Viewer (UI component)
 *
 * Responsible for displaying images and videos, handling selection
 * gestures, zooming, rotation and embedded playback controls.
 *
 * Sections: helpers, handlers (signal callbacks), lifecycle (init/dispose),
 * and public API (viewer_new, viewer_update, etc.).
 */

#include "viewer.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gst/gst.h>
#include <string.h>
#include <adwaita.h>

struct _Viewer {
    GtkBox parent_instance;
    GtkStack *stack;
    GtkOverlay *overlay;
    GtkWidget *scrolled_window;
    
    /* Image Stack for Transitions */
    GtkWidget *image_stack;
    GtkWidget *picture_1;
    GtkWidget *picture_2;
    GtkWidget *active_picture; /* The one currently visible or being transitioned to */

    GtkWidget *status_page;
    GstElement *playbin;
    
    /* State */
    GdkPixbuf *original_pixbuf;
    double zoom_level;
    gboolean fit_to_window;
    gboolean default_fit;
    int rotation_angle;

    /* Selection state (rectangle in widget coordinates relative to picture widget) */
    gboolean selection_mode; /* If TRUE, drag creates selection. If FALSE, drag is ignored (pan) */
    gboolean has_selection;
    double sel_x0, sel_y0;
    double sel_x1, sel_y1;
    GtkWidget *selection_overlay; /* draws selection rectangle */
    GtkGesture *selection_gesture;

    /* Video Controls */
    GtkWidget *video_controls_overlay; /* The box containing controls */
    GtkWidget *play_pause_btn;
    GtkWidget *seek_scale;
    GtkWidget *volume_scale;
    GtkWidget *volume_btn;
    guint video_update_id;
    gboolean is_seeking;
    double saved_volume;

    GCancellable *load_cancellable;
};

enum {
    SIGNAL_ZOOM_CHANGED,
    SIGNAL_OPEN_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(Viewer, viewer, GTK_TYPE_BOX)

/* Helpers (utility functions)
 *
 * Small internal helpers used for image layout, scaling and other
 * utility tasks. Declared up front so they can be referenced by
 * other functions defined earlier in the file.
 */
static void viewer_update_image(Viewer *self);
static double get_fit_zoom_level(Viewer *self);

/* Handlers (signal callbacks)
 *
 * These are internal callbacks connected to GTK signals (selection gestures,
 * playback controls, volume changes, etc.). We declare the prototypes here
 * so the callback implementations can appear later in the file while still
 * allowing earlier code to reference them (avoid implicit declarations).
 */
static void on_selection_draw_area(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void on_selection_drag_begin(GtkGestureDrag *gesture, gdouble start_x, gdouble start_y, Viewer *self);
static void on_selection_drag_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self);
static void on_selection_drag_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self);

static void on_seek_value_changed(GtkRange *range, Viewer *self);
static void on_volume_changed(GtkRange *range, Viewer *self);

static void
on_empty_open_clicked(GtkButton *btn, Viewer *self)
{
    g_signal_emit(self, signals[SIGNAL_OPEN_REQUESTED], 0);
}

static gboolean
on_scroll(GtkEventControllerScroll *controller, gdouble dx, gdouble dy, Viewer *self)
{
    GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    
    if ((modifiers & GDK_CONTROL_MASK)) {
        if (dy < 0) viewer_zoom_in(self);
        else if (dy > 0) viewer_zoom_out(self);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_video_update(gpointer data)
{
    Viewer *self = VIEWER(data);
    if (!self->playbin || !self->seek_scale) return G_SOURCE_REMOVE;

    gint64 pos = 0, len = 0;
    if (gst_element_query_position(self->playbin, GST_FORMAT_TIME, &pos) &&
        gst_element_query_duration(self->playbin, GST_FORMAT_TIME, &len)) {
        
        gtk_range_set_range(GTK_RANGE(self->seek_scale), 0, (double)len);
        
        g_signal_handlers_block_by_func(self->seek_scale, on_seek_value_changed, self);
        gtk_range_set_value(GTK_RANGE(self->seek_scale), (double)pos);
        g_signal_handlers_unblock_by_func(self->seek_scale, on_seek_value_changed, self);
    }
    return G_SOURCE_CONTINUE;
}

static void
on_play_pause_clicked(GtkButton *btn, Viewer *self)
{
    if (!self->playbin) return;
    
    GstState state;
    gst_element_get_state(self->playbin, &state, NULL, 0);
    
    if (state == GST_STATE_PLAYING) {
        gst_element_set_state(self->playbin, GST_STATE_PAUSED);
        gtk_button_set_icon_name(btn, "media-playback-start-symbolic");
    } else {
        gst_element_set_state(self->playbin, GST_STATE_PLAYING);
        gtk_button_set_icon_name(btn, "media-playback-pause-symbolic");
    }
}

static void
on_seek_value_changed(GtkRange *range, Viewer *self)
{
    if (!self->playbin) return;
    
    double value = gtk_range_get_value(range);
    gst_element_seek_simple(self->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, (gint64)value);
}

static void
update_volume_icon(Viewer *self)
{
    if (!self->volume_btn) return;

    gboolean is_muted = FALSE;
    if (self->playbin) {
        g_object_get(self->playbin, "mute", &is_muted, NULL);
    }
    
    double vol = gtk_range_get_value(GTK_RANGE(self->volume_scale));
    const char *icon_name = "audio-volume-high-symbolic";

    if (is_muted || vol <= 0.001) {
        icon_name = "audio-volume-muted-symbolic";
    } else if (vol < 0.33) {
        icon_name = "audio-volume-low-symbolic";
    } else if (vol < 0.66) {
        icon_name = "audio-volume-medium-symbolic";
    }

    /* Check if button is a GtkButton before setting icon - it might be initialized as something else if we didn't update init yet? No, we will update init. */
    gtk_button_set_icon_name(GTK_BUTTON(self->volume_btn), icon_name);
}

static void
on_volume_mute_clicked(GtkButton *btn, Viewer *self)
{
    if (!self->playbin) return;

    gboolean is_muted = FALSE;
    g_object_get(self->playbin, "mute", &is_muted, NULL);

    if (!is_muted) {
        /* Going muted: remember current slider volume and set slider to 0 (blocks handler to avoid auto-unmute)
           We set the playbin mute for good measure. */
        self->saved_volume = gtk_range_get_value(GTK_RANGE(self->volume_scale));
        g_signal_handlers_block_by_func(self->volume_scale, on_volume_changed, self);
        gtk_range_set_value(GTK_RANGE(self->volume_scale), 0.0);
        g_signal_handlers_unblock_by_func(self->volume_scale, on_volume_changed, self);
        g_object_set(self->playbin, "mute", TRUE, NULL);
    } else {
        /* Unmute: restore saved volume and ensure playbin is unmuted */
        g_signal_handlers_block_by_func(self->volume_scale, on_volume_changed, self);
        gtk_range_set_value(GTK_RANGE(self->volume_scale), CLAMP(self->saved_volume, 0.0, 1.0));
        g_signal_handlers_unblock_by_func(self->volume_scale, on_volume_changed, self);
        g_object_set(self->playbin, "mute", FALSE, NULL);
    }

    update_volume_icon(self);
}

static void
on_volume_changed(GtkRange *range, Viewer *self)
{
    double value = gtk_range_get_value(range);
    if (self->playbin) {
        /* Auto-unmute if sliding volume up */
        if (value > 0.01) {
             g_object_set(self->playbin, "mute", FALSE, NULL);
        }
        g_object_set(self->playbin, "volume", value, NULL);
    }

    /* Update saved volume so mute can restore a sensible level (ignore silent state) */
    if (value > 0.0)
        self->saved_volume = value;

    update_volume_icon(self);
}

/* Lifecycle (GObject)
 *
 * GObject lifecycle methods for the Viewer: instance initialization,
 * dispose and class setup are implemented below.
 */
static void
viewer_init(Viewer *self)
{
    self->zoom_level = 1.0;
    self->rotation_angle = 0;
    self->fit_to_window = TRUE;
    self->default_fit = TRUE;
    /* Start with full volume by default */
    self->saved_volume = 1.0;
    
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);

    self->overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_widget_set_hexpand(GTK_WIDGET(self->overlay), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self->overlay), TRUE);

    /* Load CSS for background colors and custom overlay style to enforce compact 40px bar */
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".viewer-dark-bg { background-color: #1e1e1e; } \n"
        ".viewer-light-bg { background-color: #fafafa; } \n"
        ".video-overlay { border-radius: 9999px; padding: 0 10px; min-height: 40px; } \n"
        ".video-overlay button { min-height: 24px; min-width: 24px; padding: 4px; margin: 2px; border-radius: 9999px; } \n"
        ".video-overlay image { margin: 0 8px; } \n"
        ".video-overlay scale { margin: 0 6px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    self->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(self->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    /* Empty State */
    self->status_page = adw_status_page_new();
    gtk_widget_add_css_class(self->status_page, "viewer-empty-state");
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->status_page), "folder-open-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(self->status_page), "No Image Loaded");
    adw_status_page_set_description(ADW_STATUS_PAGE(self->status_page), "Open a folder to start viewing.");
    
    GtkWidget *open_btn = gtk_button_new_with_label("Open Folder");
    gtk_widget_set_halign(open_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(open_btn, "pill");
    gtk_widget_add_css_class(open_btn, "suggested-action");
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_empty_open_clicked), self);
    adw_status_page_set_child(ADW_STATUS_PAGE(self->status_page), open_btn);

    gtk_stack_add_named(self->stack, self->status_page, "empty");

    /* Scrolled Window for panning */
    self->scrolled_window = gtk_scrolled_window_new();
    gtk_widget_add_css_class(self->scrolled_window, "viewer-scroller");
    gtk_widget_set_hexpand(self->scrolled_window, TRUE);
    gtk_widget_set_vexpand(self->scrolled_window, TRUE);
    
    /* Image Stack for Transitions */
    self->image_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(self->image_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(self->image_stack), 250);
    
    self->picture_1 = gtk_picture_new_for_paintable(NULL);
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture_1), FALSE);
    gtk_widget_set_halign(self->picture_1, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->picture_1, GTK_ALIGN_CENTER);
    
    self->picture_2 = gtk_picture_new_for_paintable(NULL);
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture_2), FALSE);
    gtk_widget_set_halign(self->picture_2, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->picture_2, GTK_ALIGN_CENTER);

    gtk_stack_add_named(GTK_STACK(self->image_stack), self->picture_1, "view1");
    gtk_stack_add_named(GTK_STACK(self->image_stack), self->picture_2, "view2");
    
    self->active_picture = self->picture_1;

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window), self->image_stack);
    
    gtk_stack_add_named(self->stack, self->scrolled_window, "content");
    gtk_stack_set_visible_child_name(self->stack, "empty");
    
    gtk_overlay_set_child(self->overlay, GTK_WIDGET(self->stack));

    /* Selection overlay (on top of the stack) */
    self->selection_overlay = gtk_drawing_area_new();
    gtk_widget_add_css_class(self->selection_overlay, "selection-overlay");
    gtk_widget_set_hexpand(self->selection_overlay, TRUE);
    gtk_widget_set_vexpand(self->selection_overlay, TRUE);
    gtk_overlay_add_overlay(self->overlay, self->selection_overlay);
    gtk_widget_set_visible(self->selection_overlay, FALSE);
    /* Use drawing area draw function in GTK4 */
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->selection_overlay), on_selection_draw_area, self, NULL);

    /* Gesture for selection (click-drag on picture) */
    GtkGesture *g1 = gtk_gesture_drag_new();
    g_signal_connect(g1, "drag-begin", G_CALLBACK(on_selection_drag_begin), self);
    g_signal_connect(g1, "drag-update", G_CALLBACK(on_selection_drag_update), self);
    g_signal_connect(g1, "drag-end", G_CALLBACK(on_selection_drag_end), self);
    gtk_widget_add_controller(self->picture_1, GTK_EVENT_CONTROLLER(g1));

    GtkGesture *g2 = gtk_gesture_drag_new();
    g_signal_connect(g2, "drag-begin", G_CALLBACK(on_selection_drag_begin), self);
    g_signal_connect(g2, "drag-update", G_CALLBACK(on_selection_drag_update), self);
    g_signal_connect(g2, "drag-end", G_CALLBACK(on_selection_drag_end), self);
    gtk_widget_add_controller(self->picture_2, GTK_EVENT_CONTROLLER(g2));

    /* Video controls overlay */
    /* Use a CenterBox to keep controls centered but avoid the nav buttons on the right */
    self->video_controls_overlay = gtk_center_box_new();
    gtk_widget_set_valign(self->video_controls_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(self->video_controls_overlay, 20);
    /* No margins on the overlay itself - we manage spacing with the center box children */
    
    gtk_widget_set_visible(self->video_controls_overlay, FALSE);

    /* Inner box containing the actual controls */
    GtkWidget *controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    /* Style the controls box so the bar wraps the buttons */
    gtk_widget_add_css_class(controls_box, "osd");
    /* Add custom class for rounding and compact sizing */
    gtk_widget_add_css_class(controls_box, "video-overlay");
    gtk_widget_set_size_request(controls_box, -1, 40);
    
    /* Add to center of the CenterBox */
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(self->video_controls_overlay), controls_box);

    /* Spacer on the right to reserve space for nav buttons (160px) */
    GtkWidget *right_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(right_spacer, 160, -1);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(self->video_controls_overlay), right_spacer);

    /* Play/Pause Button */
    self->play_pause_btn = gtk_button_new_from_icon_name("media-playback-pause-symbolic");
    gtk_widget_add_css_class(self->play_pause_btn, "flat");
    g_signal_connect(self->play_pause_btn, "clicked", G_CALLBACK(on_play_pause_clicked), self);
    gtk_box_append(GTK_BOX(controls_box), self->play_pause_btn);

    /* Seek Scale */
    self->seek_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_size_request(self->seek_scale, 150, -1);
    gtk_scale_set_draw_value(GTK_SCALE(self->seek_scale), FALSE);
    g_signal_connect(self->seek_scale, "value-changed", G_CALLBACK(on_seek_value_changed), self);
    gtk_box_append(GTK_BOX(controls_box), self->seek_scale);
    
    /* Volume Button (Mute Toggle) */
    self->volume_btn = gtk_button_new_from_icon_name("audio-volume-high-symbolic");
    gtk_widget_add_css_class(self->volume_btn, "flat");
    g_signal_connect(self->volume_btn, "clicked", G_CALLBACK(on_volume_mute_clicked), self);
    gtk_box_append(GTK_BOX(controls_box), self->volume_btn);

    /* Volume Scale */
    self->volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1.0, 0.05);
    gtk_range_set_value(GTK_RANGE(self->volume_scale), 1.0);
    gtk_widget_set_size_request(self->volume_scale, 50, -1);
    gtk_scale_set_draw_value(GTK_SCALE(self->volume_scale), FALSE);
    g_signal_connect(self->volume_scale, "value-changed", G_CALLBACK(on_volume_changed), self);
    gtk_box_append(GTK_BOX(controls_box), self->volume_scale);

    gtk_overlay_add_overlay(self->overlay, self->video_controls_overlay);

    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->overlay));

    /* Add Scroll Controller for Zoom */
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll);
}

static void
on_gst_error (GstBus *bus, GstMessage *msg, Viewer *self)
{
    GError *err = NULL;
    gchar *debug_info = NULL;

    gst_message_parse_error (msg, &err, &debug_info);
    g_printerr ("GStreamer Error from %s: %s\n",
                GST_OBJECT_NAME (msg->src), err->message);
    g_printerr ("Debug info: %s\n", debug_info ? debug_info : "none");
    
    g_clear_error (&err);
    g_free (debug_info);
}

static void
viewer_stop_playback (Viewer *self)
{
    if (self->video_update_id > 0) {
        g_source_remove(self->video_update_id);
        self->video_update_id = 0;
    }
    if (self->video_controls_overlay) {
        gtk_widget_set_visible(self->video_controls_overlay, FALSE);
    }

    if (self->playbin) {
        /* Detach paintable from picture to prevents access to destroyed sink */
        gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), NULL);

        g_print("Stopping playback...\n");
        GstBus *bus = gst_element_get_bus (self->playbin);
        gst_bus_remove_signal_watch (bus);
        gst_object_unref (bus);

        gst_element_set_state (self->playbin, GST_STATE_NULL);
        g_clear_object (&self->playbin);
    }
}

static void
viewer_dispose(GObject *gobject)
{
    Viewer *self = VIEWER(gobject);

    if (self->load_cancellable) {
        g_cancellable_cancel(self->load_cancellable);
        g_clear_object(&self->load_cancellable);
    }
    
    viewer_stop_playback(self);

    g_clear_object(&self->original_pixbuf);
    G_OBJECT_CLASS(viewer_parent_class)->dispose(gobject);
}

static void
viewer_class_init(ViewerClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->dispose = viewer_dispose;
    
    signals[SIGNAL_ZOOM_CHANGED] = g_signal_new("zoom-changed",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL, NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                1,
                                                G_TYPE_UINT);

    signals[SIGNAL_OPEN_REQUESTED] = g_signal_new("open-requested",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL, NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                0);
}

Viewer *
viewer_new(void)
{
    return g_object_new(TYPE_VIEWER, NULL);
}

static void
on_pixbuf_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    Viewer *self = VIEWER(user_data);
    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_finish(res, &err);

    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&err);
        g_object_unref(self);
        return;
    }

    if (!pixbuf) {
        g_warning("Failed to load image: %s", err ? err->message : "Unknown error");
        g_clear_error(&err);
        g_object_unref(self);
        return;
    }

    /* If we have a new cancellable (i.e. another load started), discard this result? 
       No, the cancellable we passed would have been triggered. 
       But self->load_cancellable might be DIFFERENT now if a new load started and we didn't use the one passed?
       In this design, we use self->load_cancellable passed to the async func. 
       If a new load started, self->load_cancellable was replaced and the old one cancelled. 
       So checking G_IO_ERROR_CANCELLED is sufficient.
    */

    g_clear_object(&self->original_pixbuf);
    self->original_pixbuf = pixbuf;
    
    self->zoom_level = 1.0;
    self->rotation_angle = 0;
    self->fit_to_window = self->default_fit;
    viewer_update_image(self);

    const char *view_name = (self->active_picture == self->picture_1) ? "view1" : "view2";
    gtk_stack_set_visible_child_name(GTK_STACK(self->image_stack), view_name);
    
    g_object_unref(self);
}

static void
on_file_read(GObject *source, GAsyncResult *res, gpointer user_data)
{
    Viewer *self = VIEWER(user_data);
    GError *err = NULL;
    GFileInputStream *stream = g_file_read_finish(G_FILE(source), res, &err);

    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&err);
        g_object_unref(self);
        return;
    }

    if (!stream) {
        g_warning("Failed to open file: %s", err ? err->message : "Unknown error");
        g_clear_error(&err);
        g_object_unref(self);
        return;
    }

    gdk_pixbuf_new_from_stream_async(G_INPUT_STREAM(stream), 
                                     self->load_cancellable, 
                                     on_pixbuf_loaded, 
                                     self);
    
    g_object_unref(stream);
}

void
viewer_load_file(Viewer *self, const char *path)
{
    /* Cancel any pending load */
    if (self->load_cancellable) {
        g_cancellable_cancel(self->load_cancellable);
        g_clear_object(&self->load_cancellable);
    }
    self->load_cancellable = g_cancellable_new();

    /* Reset selection mode and clear selection on file change */
    self->selection_mode = FALSE;
    self->has_selection = FALSE;
    gtk_widget_set_visible(self->selection_overlay, FALSE);
    gtk_widget_queue_draw(self->selection_overlay);

    if (!path) {
        viewer_stop_playback(self);
        gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), NULL);
        if (self->stack)
            gtk_stack_set_visible_child_name(self->stack, "empty");
        return;
    }

    /* Prepare transition: load into the non-active picture */
    GtkWidget *target_picture = (self->active_picture == self->picture_1) ? self->picture_2 : self->picture_1;
    self->active_picture = target_picture;

    if (self->stack)
        gtk_stack_set_visible_child_name(self->stack, "content");

    g_print("Loading file: %s\n", path);
    const char *ext = strrchr(path, '.');
    if (ext && (g_ascii_strcasecmp(ext, ".mp4") == 0 || g_ascii_strcasecmp(ext, ".mkv") == 0 ||
                g_ascii_strcasecmp(ext, ".webm") == 0 || g_ascii_strcasecmp(ext, ".avi") == 0)) {
        
        g_print("File detected as video.\n");
        if (!gst_is_initialized()) {
            g_print("Initializing GStreamer...\n");
            gst_init(NULL, NULL);
        }

        /* Stop old playback */
        viewer_stop_playback(self);

        g_print("Creating playbin...\n");
        self->playbin = gst_element_factory_make("playbin", "player");
        if (!self->playbin) {
            g_warning("Failed to create playbin");
            return;
        }
        g_object_ref_sink(self->playbin);

        /* Connect to bus for errors */
        GstBus *bus = gst_element_get_bus(self->playbin);
        gst_bus_add_signal_watch(bus);
        g_signal_connect(bus, "message::error", G_CALLBACK(on_gst_error), self);
        gst_object_unref(bus);

        g_print("Creating video sink...\n");
        GstElement *sink = gst_element_factory_make("gtk4paintablesink", "video-sink");
        if (sink) {
            /* Safely handle sink ownership */
            g_object_ref(sink); /* Keep a ref while we use it */
            
            g_object_set(self->playbin, "video-sink", sink, NULL); /* playbin takes ownership too */

            GdkPaintable *paintable = NULL;
            g_object_get(sink, "paintable", &paintable, NULL);
            
            if (paintable) {
                gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), paintable);
                if (self->fit_to_window) {
                    gtk_picture_set_can_shrink(GTK_PICTURE(self->active_picture), TRUE);
                }
                g_object_unref(paintable);
            }
            
            g_object_unref(sink); /* Release our temp ref */
        }

        /* Configure playbin */
        gchar *uri = g_filename_to_uri(path, NULL, NULL);
        g_object_set(self->playbin, "uri", uri, NULL);
        g_free(uri);

        /* Cleanup original pixbuf to save memory */
        g_clear_object(&self->original_pixbuf);

        g_print("Starting playback...\n");
        GstStateChangeReturn ret = gst_element_set_state(self->playbin, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            g_warning("Failed to start playback");
        } else {
            gtk_widget_set_visible(self->video_controls_overlay, TRUE);
            /* Reset Play button icon */
            gtk_button_set_icon_name(GTK_BUTTON(self->play_pause_btn), "media-playback-pause-symbolic");
            
            /* Apply current volume to new playbin */
            g_object_set(self->playbin, "volume", gtk_range_get_value(GTK_RANGE(self->volume_scale)), NULL);
            update_volume_icon(self);

            /* Reset seek bar */
            g_signal_handlers_block_by_func(self->seek_scale, on_seek_value_changed, self);
            gtk_range_set_value(GTK_RANGE(self->seek_scale), 0);
            g_signal_handlers_unblock_by_func(self->seek_scale, on_seek_value_changed, self);
            
            if (self->video_update_id == 0)
                self->video_update_id = g_timeout_add(200, on_video_update, self);

            /* Start Transition for Video */
            const char *view_name = (self->active_picture == self->picture_1) ? "view1" : "view2";
            gtk_stack_set_visible_child_name(GTK_STACK(self->image_stack), view_name);
        }
    
    } else {
        g_print("File detected as image.\n");
        /* Load image */
        viewer_stop_playback(self);
        
        GFile *file = g_file_new_for_path(path);
        g_object_ref(self);
        g_file_read_async(file, G_PRIORITY_DEFAULT, self->load_cancellable, on_file_read, self);
        g_object_unref(file);
    }
}

static void
viewer_update_image(Viewer *self)
{
    if (!self->original_pixbuf) return;

    GdkPixbuf *rotated = NULL;
    if (self->rotation_angle != 0) {
        rotated = gdk_pixbuf_rotate_simple(self->original_pixbuf, self->rotation_angle);
    } else {
        rotated = g_object_ref(self->original_pixbuf);
    }

    int width = gdk_pixbuf_get_width(rotated);
    int height = gdk_pixbuf_get_height(rotated);
    int stride = gdk_pixbuf_get_rowstride(rotated);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(rotated);
    
    g_object_ref(rotated);
    GBytes *bytes = g_bytes_new_with_free_func(gdk_pixbuf_get_pixels(rotated),
                                               (gsize)stride * height,
                                               (GDestroyNotify)g_object_unref,
                                               rotated);
                                               
    GdkTexture *texture = gdk_memory_texture_new(width, height,
                                                 has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                 bytes, stride);
    g_bytes_unref(bytes);

    gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), GDK_PAINTABLE(texture));
    g_clear_object(&texture);

    if (self->fit_to_window) {
        gtk_picture_set_can_shrink(GTK_PICTURE(self->active_picture), TRUE);
        gtk_widget_set_size_request(self->active_picture, -1, -1);
    } else {
        int width = gdk_pixbuf_get_width(rotated);
        int height = gdk_pixbuf_get_height(rotated);
        
        int new_width = width * self->zoom_level;
        int new_height = height * self->zoom_level;

        gtk_picture_set_can_shrink(GTK_PICTURE(self->active_picture), TRUE);
        gtk_widget_set_size_request(self->active_picture, new_width, new_height);
    }
    
    g_object_unref(rotated);
    
    /* Emit zoom changed */
    g_signal_emit(self, signals[SIGNAL_ZOOM_CHANGED], 0, viewer_get_zoom_level_percentage(self));
}

static double
get_fit_zoom_level(Viewer *self)
{
    if (!self->original_pixbuf) return 1.0;
    
    GdkPixbuf *rotated = NULL;
    /* We need rotated dimensions */
    if (self->rotation_angle != 0) {
        rotated = gdk_pixbuf_rotate_simple(self->original_pixbuf, self->rotation_angle);
    } else {
        rotated = g_object_ref(self->original_pixbuf);
    }
    
    int img_w = gdk_pixbuf_get_width(rotated);
    int img_h = gdk_pixbuf_get_height(rotated);
    g_object_unref(rotated);
    
    double alloc_w = gtk_widget_get_width(self->scrolled_window);
    double alloc_h = gtk_widget_get_height(self->scrolled_window);
    
    if (alloc_w <= 0 || alloc_h <= 0) return 1.0;
    
    double scale_x = alloc_w / (double)img_w;
    double scale_y = alloc_h / (double)img_h;
    
    return MIN(scale_x, scale_y);
}

/* Selection gestures and drawing */
static void
snapshot_draw_rect(cairo_t *cr, gpointer data)
{
    Viewer *self = VIEWER(data);
    if (!self->has_selection) return;

    double x = MIN(self->sel_x0, self->sel_x1);
    double y = MIN(self->sel_y0, self->sel_y1);
    double w = fabs(self->sel_x1 - self->sel_x0);
    double h = fabs(self->sel_y1 - self->sel_y0);

    /* Convert picture-local coords to overlay-local coords */
    double tx = 0.0, ty = 0.0;
    graphene_point_t p;
    graphene_point_init(&p, (float)x, (float)y);
    graphene_point_t res;
    
    if (gtk_widget_compute_point(self->active_picture, self->selection_overlay, &p, &res)) {
        tx = res.x;
        ty = res.y;
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.5, 1.0, 0.2);
    cairo_rectangle(cr, tx, ty, w, h);
    cairo_fill(cr);
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgba(cr, 0.0, 0.5, 1.0, 0.8);
    cairo_rectangle(cr, tx, ty, w, h);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void
on_selection_draw_area(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)area; (void)width; (void)height;
    snapshot_draw_rect(cr, user_data);
}

static void
on_selection_drag_begin(GtkGestureDrag *gesture, gdouble start_x, gdouble start_y, Viewer *self)
{
    if (!self->selection_mode) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    /* start_x/start_y are relative to the picture widget */
    self->has_selection = TRUE;
    self->sel_x0 = start_x;
    self->sel_y0 = start_y;
    self->sel_x1 = start_x;
    self->sel_y1 = start_y;
    gtk_widget_set_visible(self->selection_overlay, TRUE);
    gtk_widget_queue_draw(self->selection_overlay);
}

static void
on_selection_drag_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self)
{
    gdouble ox = 0, oy = 0;
    gtk_gesture_drag_get_offset(gesture, &ox, &oy);
    self->sel_x1 = self->sel_x0 + ox;
    self->sel_y1 = self->sel_y0 + oy;
    gtk_widget_queue_draw(self->selection_overlay);
}

static void
on_selection_drag_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self)
{
    gdouble ox = 0, oy = 0;
    gtk_gesture_drag_get_offset(gesture, &ox, &oy);
    self->sel_x1 = self->sel_x0 + ox;
    self->sel_y1 = self->sel_y0 + oy;

    /* If the selection is trivial (e.g. a simple click without drag), clear it */
    if (fabs(ox) < 5.0 && fabs(oy) < 5.0) {
        viewer_clear_selection(self);
    } else {
        gtk_widget_queue_draw(self->selection_overlay);
    }
}

/* Public selection API */
gboolean viewer_has_selection(Viewer *self)
{
    return self->has_selection;
}

GdkPixbuf *
viewer_get_selection_pixbuf(Viewer *self)
{
    if (!self->has_selection || !self->original_pixbuf) return NULL;

    GdkPixbuf *rotated = NULL;
    if (self->rotation_angle != 0)
        rotated = gdk_pixbuf_rotate_simple(self->original_pixbuf, self->rotation_angle);
    else
        rotated = g_object_ref(self->original_pixbuf);

    int img_w = gdk_pixbuf_get_width(rotated);
    int img_h = gdk_pixbuf_get_height(rotated);

    int pic_w = gtk_widget_get_width(self->active_picture);
    int pic_h = gtk_widget_get_height(self->active_picture);
    if (pic_w <= 0 || pic_h <= 0) {
        g_object_unref(rotated);
        return NULL;
    }

    double x0 = MIN(self->sel_x0, self->sel_x1);
    double y0 = MIN(self->sel_y0, self->sel_y1);
    double w = fabs(self->sel_x1 - self->sel_x0);
    double h = fabs(self->sel_y1 - self->sel_y0);

    /* Calculate displayed image dimensions and offsets within the widget */
    double aspect_img = (double)img_w / (double)img_h;
    double aspect_pic = (double)pic_w / (double)pic_h;
    
    double draw_w, draw_h;
    double off_x, off_y;

    if (aspect_img > aspect_pic) {
        /* Image is wider than widget (relative to aspect), constrained by width */
        draw_w = pic_w;
        draw_h = pic_w / aspect_img;
        off_x = 0;
        off_y = (pic_h - draw_h) / 2.0;
    } else {
        /* Image is taller than widget, constrained by height */
        draw_h = pic_h;
        draw_w = pic_h * aspect_img;
        off_y = 0;
        off_x = (pic_w - draw_w) / 2.0;
    }

    /* Map selection coordinates to image coordinates */
    /* (x_widget - offset) / draw_size * orig_size */
    
    double scale = (double)img_w / draw_w; /* = img_h / draw_h */

    int ix = (int)((x0 - off_x) * scale);
    int iy = (int)((y0 - off_y) * scale);
    int iw = (int)(w * scale);
    int ih = (int)(h * scale);

    /* Clamp to image bounds */
    if (ix < 0) { iw += ix; ix = 0; }
    if (iy < 0) { ih += iy; iy = 0; }
    
    /* Further clamping */
    if (ix > img_w) ix = img_w;
    if (iy > img_h) iy = img_h;
    if (ix + iw > img_w) iw = img_w - ix;
    if (iy + ih > img_h) ih = img_h - iy;

    if (iw <= 0 || ih <= 0) {
        g_object_unref(rotated);
        return NULL;
    }

    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(rotated, ix, iy, iw, ih);
    GdkPixbuf *copy = gdk_pixbuf_copy(sub);
    g_object_unref(sub);
    g_object_unref(rotated);
    return copy;
}

void
viewer_set_selection_mode(Viewer *self, gboolean enabled)
{
    if (enabled && !self->original_pixbuf) {
        /* Cannot enable selection mode on non-images (e.g. videos) */
        enabled = FALSE;
    }
    self->selection_mode = enabled;
    if (enabled && self->active_picture) {
        gtk_widget_set_cursor_from_name(self->active_picture, "crosshair");
    } else if (self->active_picture) {
        gtk_widget_set_cursor(self->active_picture, NULL);
    }
}

gboolean
viewer_get_selection_mode(Viewer *self)
{
    return self->selection_mode;
}

void
viewer_clear_selection(Viewer *self)
{
    if (!self->has_selection) return;
    self->has_selection = FALSE;
    gtk_widget_set_visible(self->selection_overlay, FALSE);
    gtk_widget_queue_draw(self->selection_overlay);
}


void viewer_zoom_in(Viewer *self) {
    if (self->fit_to_window) {
        self->zoom_level = get_fit_zoom_level(self);
        self->fit_to_window = FALSE;
    }
    self->zoom_level *= 1.1;
    viewer_update_image(self);
}

void viewer_zoom_out(Viewer *self) {
    if (self->fit_to_window) {
        self->fit_to_window = FALSE;
        /* Do not recalc fit zoom here, just start from current effective zoom? 
           If was fit, we should probably start from fit zoom. */
        self->zoom_level = get_fit_zoom_level(self);
    }
    self->zoom_level /= 1.1;
    viewer_update_image(self);
}

void viewer_set_fit_to_window(Viewer *self, gboolean fit) {
    self->fit_to_window = fit;
    
    if (self->playbin) {
        gtk_picture_set_can_shrink(GTK_PICTURE(self->active_picture), fit);
        if (fit) {
            gtk_widget_set_size_request(self->active_picture, -1, -1);
        }
        /* If not fit (100%), can_shrink=FALSE forces natural size */
    } else {
        viewer_update_image(self);
    }
}

void viewer_zoom_reset(Viewer *self) {
    self->fit_to_window = FALSE;
    self->zoom_level = 1.0;
    viewer_update_image(self);
}

void viewer_set_default_fit(Viewer *self, gboolean fit) {
    self->default_fit = fit;
}

static GtkCssProvider *viewer_css_provider(void)
{
    static GtkCssProvider *provider = NULL;
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider,
            ".viewer-scroller.dark > viewport { background-color: #404040; }\n"
            ".viewer-scroller.light > viewport { background-color: #f5f5f5; }\n"
            ".viewer-empty-state.dark { background-color: #404040; color: #eeeeee; }\n"
            ".viewer-empty-state.light { background-color: #f5f5f5; color: #202020; }\n"
            ".viewer-empty-state.dark image { color: #eeeeee; }\n"
            ".viewer-empty-state.light image { color: #202020; }\n"
        );
        GdkDisplay *display = gdk_display_get_default();
        if (display)
            gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    return provider;
}

void viewer_set_dark_background(Viewer *self, gboolean dark)
{
    GtkCssProvider *prov = viewer_css_provider();
    (void)prov; /* provider installed globally */

    const char *add = dark ? "dark" : "light";
    const char *remove = dark ? "light" : "dark";

    if (self->scrolled_window) {
        gtk_widget_remove_css_class(self->scrolled_window, remove);
        gtk_widget_add_css_class(self->scrolled_window, add);
    }
    if (self->status_page) {
        gtk_widget_remove_css_class(self->status_page, remove);
        gtk_widget_add_css_class(self->status_page, add);
    }
}

void viewer_rotate_cw(Viewer *self) {
    switch(self->rotation_angle) {
        case 0: self->rotation_angle = 270; break;
        case 270: self->rotation_angle = 180; break;
        case 180: self->rotation_angle = 90; break;
        case 90: self->rotation_angle = 0; break;
    }
    viewer_update_image(self);
}

void viewer_rotate_ccw(Viewer *self) {
     switch(self->rotation_angle) {
        case 0: self->rotation_angle = 90; break;
        case 90: self->rotation_angle = 180; break;
        case 180: self->rotation_angle = 270; break;
        case 270: self->rotation_angle = 0; break;
    }
    viewer_update_image(self);
}

guint viewer_get_zoom_level_percentage(Viewer *self) {
    if (self->fit_to_window) return (guint)(get_fit_zoom_level(self) * 100);
    return (guint)(self->zoom_level * 100);
}

