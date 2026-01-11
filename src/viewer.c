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
#include <math.h>
#include <adwaita.h>
#include "archive.h"

/* Animation pipeline removed to simplify the code; zooming will be reimplemented later. */

struct _Viewer {
    GtkBox parent_instance;
    GtkStack *stack;
    GtkOverlay *overlay;
    GtkWidget *scrolled_window;
    
    /* Animation pipeline removed: no active animation state */
    
    /* Image Stack for Transitions */
    GtkWidget *image_stack;
    GtkWidget *picture_1;
    GtkWidget *picture_2;
    GtkWidget *active_picture; /* The one currently visible or being transitioned to */

    GtkWidget *status_page;
    GstElement *playbin;
    
    /* State */
    GdkPixbuf *original_pixbuf;
    /* Cached full-resolution texture derived from rotated pixbuf to avoid expensive
     * scaling each frame during animated zooms. We regenerate when the source pixbuf
     * or rotation changes.
     * preview_texture: a downscaled version used during animated zoom to reduce GPU
     * scaling costs for very large images. */
    GdkTexture *original_texture;
    GdkTexture *preview_texture;
    int original_texture_rotation_angle;
    double zoom_level;
    gboolean fit_to_window;
    gboolean fit_to_width;
    gboolean default_fit;
    int rotation_angle;

    /* Selection state (rectangle in widget coordinates relative to picture widget) */
    gboolean selection_mode; /* If TRUE, drag creates selection. If FALSE, drag is ignored (pan) */
    gboolean has_selection;
    double sel_x0, sel_y0;
    double sel_x1, sel_y1;
    GtkWidget *selection_overlay; /* draws selection rectangle */
    GtkGesture *selection_gesture;

    /* Debug overlay for allocation inspection */
    GtkWidget *debug_label;

    /* Panning state */
    double pan_start_adj_h;
    double pan_start_adj_v;

    /* Pending scroll restoration (for zooming) */
    gboolean has_pending_center;
    double pending_center_x;
    double pending_center_y;
    int center_retry_count;

    int last_viewport_width;

    /* Video Controls */
    GtkWidget *video_controls_overlay; /* The box containing controls */
    GtkWidget *play_pause_btn;
    GtkWidget *seek_scale;
    GtkWidget *volume_scale;
    GtkWidget *volume_btn;
    guint video_update_id;
    gboolean is_seeking;
    double saved_volume;

    /* Scroll accumulation for smooth zoom */
    double scroll_accumulator;
    guint scroll_timeout_id;
    GCancellable *load_cancellable;
};

/* scroll_timeout_cb removed: unused while scroll-wheel zoom is disabled. */

enum {
    SIGNAL_ZOOM_CHANGED,
    SIGNAL_OPEN_REQUESTED,
    SIGNAL_PLAYBACK_CHANGED,
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
static double get_fit_width_zoom(Viewer *self);
static void viewer_set_zoom_level_internal(Viewer *self, double target_scale, gboolean center);

/* Animation helpers */
/* Animation helpers removed. */

/* Debug helper: schedule after layout to inspect active picture allocation */
static gboolean update_alloc_overlay(gpointer user_data);

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

static void on_pan_drag_begin(GtkGestureDrag *gesture, gdouble start_x, gdouble start_y, Viewer *self);
static void on_pan_drag_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self);
static void on_pan_drag_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self);

static void on_seek_value_changed(GtkRange *range, Viewer *self);
static void on_volume_changed(GtkRange *range, Viewer *self);

/* Deferred pointer-anchored scroll helper removed: unused while scroll-wheel zoom is disabled. */

/* Animation state removed. */

/* Zoom functions removed as part of the simplification; reimplement cleanly later. */

/* Animation tick removed. */

static void
on_empty_open_clicked(GtkButton *btn, Viewer *self)
{
    g_signal_emit(self, signals[SIGNAL_OPEN_REQUESTED], 0);
}

/* on_scroll removed: scroll-wheel zoom is disabled while debugging anchored zoom behavior. */


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
        g_signal_emit(self, signals[SIGNAL_PLAYBACK_CHANGED], 0, FALSE);
    } else {
        gst_element_set_state(self->playbin, GST_STATE_PLAYING);
        gtk_button_set_icon_name(btn, "media-playback-pause-symbolic");
        g_signal_emit(self, signals[SIGNAL_PLAYBACK_CHANGED], 0, TRUE);
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

static void
on_viewport_resize(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    Viewer *self = VIEWER(user_data);
    int w = 0;
    if (GTK_IS_ADJUSTMENT(object)) {
        w = (int)round(gtk_adjustment_get_page_size(GTK_ADJUSTMENT(object)));
    } else if (GTK_IS_WIDGET(object)) {
        w = gtk_widget_get_width(GTK_WIDGET(object));
    }

    if (w <= 0) return;

    if (w != self->last_viewport_width) {
        self->last_viewport_width = w;
        if (self->fit_to_width) viewer_update_image(self);
    }
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
    self->scroll_accumulator = 0.0;
    self->scroll_timeout_id = 0;
    /* animation pipeline removed; no animation state */
    self->original_texture = NULL;
    self->original_texture_rotation_angle = -1;

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
    g_signal_connect(self->scrolled_window, "notify::allocation", G_CALLBACK(on_viewport_resize), self);
    
    /* Also listen for page_size changes (e.g. scrollbar appearance) to refine fit-to-width */
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_viewport_resize), self);
    
    /* Image Stack for Transitions */
    self->image_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(self->image_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(self->image_stack), 250);
    /* Make sure the scrolled content can grow to its natural size for scrolling. */
    gtk_widget_set_hexpand(self->image_stack, FALSE);
    gtk_widget_set_vexpand(self->image_stack, FALSE);
    gtk_widget_set_halign(self->image_stack, GTK_ALIGN_START);
    gtk_widget_set_valign(self->image_stack, GTK_ALIGN_START);
    
    self->picture_1 = gtk_picture_new_for_paintable(NULL);
    gtk_picture_set_content_fit(GTK_PICTURE(self->picture_1), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(self->picture_1), FALSE);
    gtk_widget_set_halign(self->picture_1, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->picture_1, GTK_ALIGN_CENTER);
    
    self->picture_2 = gtk_picture_new_for_paintable(NULL);
    gtk_picture_set_content_fit(GTK_PICTURE(self->picture_2), GTK_CONTENT_FIT_CONTAIN);
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

    /* Debug label overlay to show current zoom and allocation */
    self->debug_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->debug_label, "debug-overlay");
    gtk_widget_set_halign(self->debug_label, GTK_ALIGN_START);
    gtk_widget_set_valign(self->debug_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(self->debug_label, 8);
    gtk_widget_set_margin_top(self->debug_label, 8);
    gtk_overlay_add_overlay(self->overlay, self->debug_label);
    gtk_widget_set_visible(self->debug_label, FALSE);

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

    /* Scroll-wheel zooming temporarily disabled. Use buttons for zoom in/out.
       To re-enable, restore the scroll controller and handler (see history). */
    /* GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll); */
    
    /* Add Pan Gesture to ScrolledWindow (Stable reference for panning) */
    GtkGesture *pan = gtk_gesture_drag_new();
    g_signal_connect(pan, "drag-begin", G_CALLBACK(on_pan_drag_begin), self);
    g_signal_connect(pan, "drag-update", G_CALLBACK(on_pan_drag_update), self);
    g_signal_connect(pan, "drag-end", G_CALLBACK(on_pan_drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self->scrolled_window), GTK_EVENT_CONTROLLER(pan));

    /* Also attach pan gesture to the picture widgets so left-click drag on the image
       pans the view directly (more intuitive). */
    GtkGesture *ppan1 = gtk_gesture_drag_new();
    g_signal_connect(ppan1, "drag-begin", G_CALLBACK(on_pan_drag_begin), self);
    g_signal_connect(ppan1, "drag-update", G_CALLBACK(on_pan_drag_update), self);
    g_signal_connect(ppan1, "drag-end", G_CALLBACK(on_pan_drag_end), self);
    gtk_widget_add_controller(self->picture_1, GTK_EVENT_CONTROLLER(ppan1));

    GtkGesture *ppan2 = gtk_gesture_drag_new();
    g_signal_connect(ppan2, "drag-begin", G_CALLBACK(on_pan_drag_begin), self);
    g_signal_connect(ppan2, "drag-update", G_CALLBACK(on_pan_drag_update), self);
    g_signal_connect(ppan2, "drag-end", G_CALLBACK(on_pan_drag_end), self);
    gtk_widget_add_controller(self->picture_2, GTK_EVENT_CONTROLLER(ppan2));
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

        g_debug("Stopping playback...");
        GstBus *bus = gst_element_get_bus (self->playbin);
        gst_bus_remove_signal_watch (bus);
        gst_object_unref (bus);

        gst_element_set_state (self->playbin, GST_STATE_NULL);
        g_clear_object (&self->playbin);

        /* Notify listeners that playback stopped */
        g_signal_emit(self, signals[SIGNAL_PLAYBACK_CHANGED], 0, FALSE);
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
    g_clear_object(&self->original_texture);
    g_clear_object(&self->preview_texture);
    self->original_texture_rotation_angle = -1;
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

    signals[SIGNAL_PLAYBACK_CHANGED] = g_signal_new("playback-changed",
                                                    G_TYPE_FROM_CLASS(klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL, NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    1,
                                                    G_TYPE_BOOLEAN);
}

Viewer *
viewer_new(void)
{
    return g_object_new(TYPE_VIEWER, NULL);
}

/* Public helper: return TRUE if currently playing (playbin is in PLAYING state) */
gboolean
viewer_is_playing(Viewer *self)
{
    if (!self || !self->playbin) return FALSE;

    GstState state;
    gst_element_get_state(self->playbin, &state, NULL, 0);
    return state == GST_STATE_PLAYING;
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

    /* Replace stored pixbuf with the newly loaded one and invalidate cached textures */
    g_clear_object(&self->original_pixbuf);
    g_clear_object(&self->original_texture);
    g_clear_object(&self->preview_texture);
    self->original_texture_rotation_angle = -1;

    /* Take ownership of the loaded pixbuf */
    self->original_pixbuf = pixbuf;

    self->zoom_level = 1.0;
    self->rotation_angle = 0;
    /* Preserve fit-to-width across page loads (e.g. comics). */
    self->fit_to_window = self->fit_to_width ? FALSE : self->default_fit;

    /* Update the image now that we have a pixbuf */
    viewer_update_image(self);

    const char *view_name = (self->active_picture == self->picture_1) ? "view1" : "view2";
    gtk_stack_set_visible_child_name(GTK_STACK(self->image_stack), view_name);
    
    g_object_unref(self);
}

static void
on_archive_entry_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    Viewer *self = VIEWER(user_data);
    GError *err = NULL;
    GBytes *bytes = archive_read_entry_bytes_finish(res, &err);

    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&err);
        g_object_unref(self);
        return;
    }

    if (!bytes) {
        g_warning("Failed to read archive entry: %s", err ? err->message : "unknown");
        g_clear_error(&err);
        /* If we fail, we still own the ref to self, so unref it */
        g_object_unref(self);
        return;
    }

    g_debug("Got %zu bytes from archive entry (async)", g_bytes_get_size(bytes));

    /* Create memory input stream from a copy of the bytes */
    gsize size = g_bytes_get_size(bytes);
    const guint8 *data = g_bytes_get_data(bytes, NULL);
    guint8 *copy = g_malloc(size);
    memcpy(copy, data, size);
    GInputStream *mem = g_memory_input_stream_new_from_data(copy, (gssize)size, g_free);
    g_bytes_unref(bytes);

    /* Feed into gdk-pixbuf async loader */
    /* Pass ownership of 'self' to the next callback */
    gdk_pixbuf_new_from_stream_async(G_INPUT_STREAM(mem), 
                                     self->load_cancellable, 
                                     on_pixbuf_loaded, 
                                     self);
    g_object_unref(mem);
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

    g_debug("Loading file: %s", path);

    /* Archive virtual path handling: archive://<archive_path>::<entry_name> */
    if (g_str_has_prefix(path, "archive://")) {
        const char *sep = strstr(path, "::");
        if (!sep) {
            g_warning("Invalid archive path: %s", path);
            return;
        }
        size_t archive_len = sep - (path + strlen("archive://"));
        char *archive_path = g_strndup(path + strlen("archive://"), archive_len);
        char *entry_name = g_strdup(sep + 2);

        g_debug("Loading image from archive '%s' entry '%s'", archive_path, entry_name);

        g_object_ref(self);
        archive_read_entry_bytes_async(archive_path, entry_name, self->load_cancellable, on_archive_entry_loaded, self);

        g_free(archive_path);
        g_free(entry_name);
        return;
    }

    const char *ext = strrchr(path, '.');
    if (ext && (g_ascii_strcasecmp(ext, ".mp4") == 0 || g_ascii_strcasecmp(ext, ".mkv") == 0 ||
                g_ascii_strcasecmp(ext, ".webm") == 0 || g_ascii_strcasecmp(ext, ".avi") == 0)) {
        
        g_debug("File detected as video.");
        if (!gst_is_initialized()) {
            g_debug("Initializing GStreamer...");
            gst_init(NULL, NULL);
        }

        /* Stop old playback */
        viewer_stop_playback(self);

        g_debug("Creating playbin...");
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

        g_debug("Creating video sink...");
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
        g_clear_object(&self->original_texture);
        g_clear_object(&self->preview_texture);
        self->original_texture_rotation_angle = -1;

        g_debug("Starting playback...");
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

            /* Emit playback changed */
            g_signal_emit(self, signals[SIGNAL_PLAYBACK_CHANGED], 0, TRUE);
        }
    
    } else {
        g_debug("File detected as image.");
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

    gboolean has_alpha = gdk_pixbuf_get_has_alpha(rotated);
    
    /* Use a cached full-resolution texture derived from the rotated pixbuf.
       Avoid CPU-intensive per-frame scaling for animated zoom; instead set the
       widget size request and let the GPU/texture scaling handle the visual
       scaling. */
    if (self->original_texture == NULL || self->original_texture_rotation_angle != self->rotation_angle) {
        /* (Re)create cached texture for current rotation */
        g_clear_object(&self->original_texture);

        int rwidth = gdk_pixbuf_get_width(rotated);
        int rheight = gdk_pixbuf_get_height(rotated);
        int rstride = gdk_pixbuf_get_rowstride(rotated);

        g_object_ref(rotated);
        GBytes *rbytes = g_bytes_new_with_free_func(gdk_pixbuf_get_pixels(rotated),
                                                    (gsize)rstride * rheight,
                                                    (GDestroyNotify)g_object_unref,
                                                    rotated);

        self->original_texture = gdk_memory_texture_new(rwidth, rheight,
                                                         has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                         rbytes, rstride);
        g_bytes_unref(rbytes);
        self->original_texture_rotation_angle = self->rotation_angle;

        /* Create a downscaled preview texture for very large images to use during
           animated zoom. This reduces GPU memory bandwidth and can make animation
           smoother on systems where scaling huge textures is expensive. */
        g_clear_object(&self->preview_texture);
        const int MAX_PREVIEW_DIM = 3000;
        if (rwidth > MAX_PREVIEW_DIM || rheight > MAX_PREVIEW_DIM) {
            double scale = (double)MAX_PREVIEW_DIM / (double)MAX(rwidth, rheight);
            int pw = MAX(1, (int)(rwidth * scale));
            int ph = MAX(1, (int)(rheight * scale));

            /* Create a scaled-down pixbuf and make a texture from it */
            GdkPixbuf *preview_pix = gdk_pixbuf_scale_simple(rotated, pw, ph, GDK_INTERP_BILINEAR);
            int pstride = gdk_pixbuf_get_rowstride(preview_pix);
            g_object_ref(preview_pix);
            GBytes *pbytes = g_bytes_new_with_free_func(gdk_pixbuf_get_pixels(preview_pix), (gsize)pstride * ph,
                                                        (GDestroyNotify)g_object_unref, preview_pix);
            self->preview_texture = gdk_memory_texture_new(pw, ph,
                                                           has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                           pbytes, pstride);
            g_bytes_unref(pbytes);
        }
    }

    /* Choose texture depending on whether an animated zoom is in progress */
    GdkTexture *texture_to_use = self->original_texture;
    if (self->preview_texture)
        texture_to_use = self->preview_texture; /* prefer preview if available */


    /* For manual zoom, set the picture widget's size request to the desired
       logical size and let GTK scale the cached texture at draw time. */
    if (!self->fit_to_window) {
        /* If fit-to-width is active, update zoom level to match current viewport width */
        if (self->fit_to_width) {
            self->zoom_level = get_fit_width_zoom(self);
        }

        int width = gdk_pixbuf_get_width(rotated);
        int height = gdk_pixbuf_get_height(rotated);
        int new_width = MAX(1, (int)(width * self->zoom_level));
        int new_height = MAX(1, (int)(height * self->zoom_level));

        /* Decide whether content is smaller than viewport and allow shrinking then */
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        double page_x = gtk_adjustment_get_page_size(hadj);
        double page_y = gtk_adjustment_get_page_size(vadj);

          /* The scrolled window bases its scrollable area on its direct child
            * (image_stack). Size the stack to the target scaled dimensions.
            *
            * IMPORTANT: allow GtkPicture to shrink so zoom levels < 1.0 actually
            * take effect (fit-to-width for large images is typically < 1.0).
            */
        gtk_widget_set_size_request(self->image_stack, new_width, new_height);

                gtk_picture_set_can_shrink(GTK_PICTURE(self->active_picture), TRUE);
                gtk_widget_set_size_request(self->active_picture, new_width, new_height);

        /* If content is smaller than viewport, center the picture widget so it does
         * not stick to the top-left when zoomed out. Otherwise fill and allow scrolling.
         *
         * Important: do NOT toggle halign/valign while an animation is in progress --
         * switching alignment mid-animation causes the picture origin to jump which
         * breaks anchored zoom behaviour. We only update alignment when not animating.
         */
        if (new_width <= (int)page_x && new_height <= (int)page_y) {
            gtk_widget_set_halign(self->image_stack, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(self->image_stack, GTK_ALIGN_CENTER);
        } else {
            /* Do NOT use FILL here or the stack will stay viewport-sized. */
            gtk_widget_set_halign(self->image_stack, GTK_ALIGN_START);
            gtk_widget_set_valign(self->image_stack, GTK_ALIGN_START);
        }

        /* The picture should fill the stack allocation. */
        gtk_widget_set_halign(self->active_picture, GTK_ALIGN_FILL);
        gtk_widget_set_valign(self->active_picture, GTK_ALIGN_FILL);

        /* Ensure we always set a paintable: prefer preview (during animation), then full texture,
           finally fall back to creating a temporary texture from the rotated pixbuf. */
        if (texture_to_use) {
            gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), GDK_PAINTABLE(texture_to_use));
            g_debug("viewer_update_image: using %s texture for paintable", (self->preview_texture) ? "preview" : "original");
        } else {
            /* Fallback: construct a temporary texture from the rotated pixbuf so the view is never empty */
            g_warning("viewer_update_image: texture_to_use is NULL, falling back to temporary texture");
            g_object_ref(rotated);
            GBytes *tmpbytes = g_bytes_new_with_free_func(gdk_pixbuf_get_pixels(rotated), (gsize)gdk_pixbuf_get_rowstride(rotated) * gdk_pixbuf_get_height(rotated),
                                                          (GDestroyNotify)g_object_unref, rotated);
            GdkTexture *tmp_tex = gdk_memory_texture_new(gdk_pixbuf_get_width(rotated), gdk_pixbuf_get_height(rotated),
                                                         has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                         tmpbytes, gdk_pixbuf_get_rowstride(rotated));
            g_bytes_unref(tmpbytes);
            if (tmp_tex) {
                gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), GDK_PAINTABLE(tmp_tex));
                g_debug("viewer_update_image: using temporary texture paintable (w=%d h=%d)", gdk_pixbuf_get_width(rotated), gdk_pixbuf_get_height(rotated));
                g_clear_object(&tmp_tex);
            } else {
                g_warning("viewer_update_image: fallback temporary texture creation failed");
            }
        }

        g_debug("viewer_update_image: requested=(%d,%d) zoom=%f upper=(%f,%f) page=(%f,%f)",
                new_width, new_height, self->zoom_level, gtk_adjustment_get_upper(hadj), gtk_adjustment_get_upper(vadj), page_x, page_y);
    } else {
        /* Fit to window: let the picture shrink to fit and remove explicit size request */
        if (texture_to_use) {
            gtk_picture_set_paintable(GTK_PICTURE(self->active_picture), GDK_PAINTABLE(texture_to_use));
            g_debug("viewer_update_image: using %s texture for paintable (fit)", (self->preview_texture) ? "preview" : "original");
        } else {
            g_warning("viewer_update_image: no texture available to set for fit-to-window");
        }
        gtk_picture_set_can_shrink(GTK_PICTURE(self->active_picture), TRUE);
        gtk_widget_set_size_request(self->active_picture, -1, -1);

        gtk_widget_set_size_request(self->image_stack, -1, -1);
        gtk_widget_set_halign(self->image_stack, GTK_ALIGN_FILL);
        gtk_widget_set_valign(self->image_stack, GTK_ALIGN_FILL);
    }

    
    g_object_unref(rotated);
    
    /* Emit zoom changed */
    g_signal_emit(self, signals[SIGNAL_ZOOM_CHANGED], 0, viewer_get_zoom_level_percentage(self));

    /* Schedule an idle to inspect the active picture allocation after layout settles */
    g_idle_add(update_alloc_overlay, self);
}

static gboolean
update_alloc_overlay(gpointer user_data)
{
    Viewer *self = VIEWER(user_data);
    if (!self->active_picture) return G_SOURCE_REMOVE;

    /* Query current allocation of active picture */
    int alloc_w = gtk_widget_get_width(self->active_picture);
    int alloc_h = gtk_widget_get_height(self->active_picture);

    int orig_w_disp = 0, orig_h_disp = 0;
    if (self->original_pixbuf) {
        /* Determine "logical" dimensions of the image being displayed (rotated) */
        int w = gdk_pixbuf_get_width(self->original_pixbuf);
        int h = gdk_pixbuf_get_height(self->original_pixbuf);
        if (self->rotation_angle == 90 || self->rotation_angle == 270) {
            orig_w_disp = h;
            orig_h_disp = w;
        } else {
            orig_w_disp = w;
            orig_h_disp = h;
        }
    }

    /* Check if scrolling bounds have caught up before restoring scroll.
     * In GTK4, widgets inside GtkScrolledWindow may still be allocated at the
     * viewport size; the authoritative "content size" is adjustment->upper.
     */
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    double page_x = gtk_adjustment_get_page_size(hadj);
    double page_y = gtk_adjustment_get_page_size(vadj);
    double upper_h = gtk_adjustment_get_upper(hadj);
    double upper_v = gtk_adjustment_get_upper(vadj);

    if (self->has_pending_center && !self->fit_to_window) {
        double target_upper_h = MAX(page_x, (double)orig_w_disp * self->zoom_level);
        double target_upper_v = MAX(page_y, (double)orig_h_disp * self->zoom_level);

        if (fabs(upper_h - target_upper_h) > 5.0 || fabs(upper_v - target_upper_v) > 5.0) {
            self->center_retry_count++;
            if (self->center_retry_count < 200) {
                return G_SOURCE_CONTINUE;
            }
            /* Proceed with best effort if bounds never update. */
        }
    }

        g_debug("alloc_overlay: zoom=%f allocated=(%d,%d) original_disp=(%d,%d)",
            self->zoom_level, alloc_w, alloc_h, orig_w_disp, orig_h_disp);

    /* Apply pending center scroll if needed (deferred until after layout) */
    if (self->has_pending_center) {
        self->has_pending_center = FALSE;

        /* page_x/page_y/upper_h/upper_v already computed above */
        /* Calculate desired scroll position in the NEW scale */
        double new_val_x = self->pending_center_x * self->zoom_level - page_x / 2.0;
        double new_val_y = self->pending_center_y * self->zoom_level - page_y / 2.0;
        
        double max_x = MAX(0, upper_h - page_x);
        double max_y = MAX(0, upper_v - page_y);
        
        new_val_x = CLAMP(new_val_x, 0.0, max_x);
        new_val_y = CLAMP(new_val_y, 0.0, max_y);
        
        gtk_adjustment_set_value(hadj, new_val_x);
        gtk_adjustment_set_value(vadj, new_val_y);
        
        g_debug("update_alloc_overlay: restored center to scroll pos (%f, %f) upper=(%f,%f)",
            new_val_x, new_val_y, upper_h, upper_v);
    }

    if (self->debug_label) {
        gchar *txt = g_strdup_printf("zoom=%.3f\nalloc=%dx%d\norig=%dx%d",
                                     self->zoom_level, alloc_w, alloc_h, orig_w_disp, orig_h_disp);
        gtk_label_set_text(GTK_LABEL(self->debug_label), txt);
        g_free(txt);
    }

    return G_SOURCE_REMOVE;
}

static double
get_fit_zoom_level(Viewer *self)
{
    if (!self->original_pixbuf) return 1.0;
    
    int img_w = gdk_pixbuf_get_width(self->original_pixbuf);
    int img_h = gdk_pixbuf_get_height(self->original_pixbuf);
    
    /* Swap dimensions if rotated 90 or 270 degrees */
    if (self->rotation_angle == 90 || self->rotation_angle == 270) {
        int temp = img_w;
        img_w = img_h;
        img_h = temp;
    }
    
    /* Fit zoom calculation (debug prints removed) */
    
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

/* Pan Gesture Callbacks */
static void
on_pan_drag_begin(GtkGestureDrag *gesture, gdouble start_x, gdouble start_y, Viewer *self)
{
    if (self->selection_mode) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    /* Start Panning: capture current scroll values */
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    
    self->pan_start_adj_h = gtk_adjustment_get_value(hadj);
    self->pan_start_adj_v = gtk_adjustment_get_value(vadj);
    
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "grabbing");
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_pan_drag_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self)
{
    if (self->selection_mode) return;
    
    /* Pan update */
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    
    /* New pos = Start - Offset (dragging content moves viewport opposite) */
    gtk_adjustment_set_value(hadj, self->pan_start_adj_h - offset_x);
    gtk_adjustment_set_value(vadj, self->pan_start_adj_v - offset_y);
}

static void
on_pan_drag_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y, Viewer *self)
{
    if (!self->selection_mode) {
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), NULL); 
    }
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


/* Helper: compute zoom scale that fits image width to viewport width */
static double
get_fit_width_zoom(Viewer *self)
{
    if (!self->original_pixbuf) return 1.0;
    int img_w = gdk_pixbuf_get_width(self->original_pixbuf);
    int img_h = gdk_pixbuf_get_height(self->original_pixbuf);
    if (self->rotation_angle == 90 || self->rotation_angle == 270) {
        int tmp = img_w; img_w = img_h; img_h = tmp;
    }
    
    /* Use page size from adjustment to account for scrollbars */
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    double alloc_w = gtk_adjustment_get_page_size(hadj);
    
    /* Fallback if page size is invalid */
    if (alloc_w <= 0) alloc_w = gtk_widget_get_width(self->scrolled_window);

    if (alloc_w <= 0 || img_w <= 0) return 1.0;
    return alloc_w / (double)img_w;
}

/* Internal setter that preserves viewport center when requested */
static void
viewer_set_zoom_level_internal(Viewer *self, double target_scale, gboolean center)
{
    double before_scale = self->fit_to_window ? get_fit_zoom_level(self) : self->zoom_level;
    double min_scale = get_fit_zoom_level(self);
    double max_scale = 10.0; /* keep manual zoom bounded but usable */
    target_scale = MAX(min_scale, MIN(max_scale, target_scale));

    if (fabs(target_scale - before_scale) < 1e-6) return;

    double content_center_x = 0.0, content_center_y = 0.0;
    if (center) {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        double page_x = gtk_adjustment_get_page_size(hadj);
        double page_y = gtk_adjustment_get_page_size(vadj);
        double val_x = gtk_adjustment_get_value(hadj);
        double val_y = gtk_adjustment_get_value(vadj);
        content_center_x = (val_x + page_x / 2.0) / before_scale;
        content_center_y = (val_y + page_y / 2.0) / before_scale;
        
        self->pending_center_x = content_center_x;
        self->pending_center_y = content_center_y;
        self->has_pending_center = TRUE;
        self->center_retry_count = 0;
    } else {
        self->has_pending_center = FALSE;
    }

    /* Clear any fit flags - we are now in manual zoom mode */
    self->fit_to_window = FALSE;
    self->fit_to_width = FALSE;
    self->zoom_level = target_scale;
    viewer_update_image(self);
}

void viewer_zoom_in(Viewer *self) {
    /* discrete percentage steps */
    const double steps[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 5.0, 7.5, 10.0};
    double cur = self->fit_to_window ? get_fit_zoom_level(self) : self->zoom_level;
    for (guint i = 0; i < G_N_ELEMENTS(steps); i++) {
        if (steps[i] > cur + 1e-6) {
            viewer_set_zoom_level_internal(self, steps[i], TRUE);
            return;
        }
    }
    /* already at or above max - clamp */
    viewer_set_zoom_level_internal(self, steps[G_N_ELEMENTS(steps)-1], TRUE);
}

void viewer_zoom_out(Viewer *self) {
    const double steps[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 5.0, 7.5, 10.0};
    double cur = self->fit_to_window ? get_fit_zoom_level(self) : self->zoom_level;
    for (gint i = G_N_ELEMENTS(steps) - 1; i >= 0; i--) {
        if (steps[i] < cur - 1e-6) {
            viewer_set_zoom_level_internal(self, steps[i], TRUE);
            return;
        }
    }
    /* If we fell below the smallest step, set to fit if that is smaller */
    double fit = get_fit_zoom_level(self);
    if (fit < steps[0]) {
        viewer_set_zoom_level_internal(self, fit, TRUE);
    } else {
        viewer_set_zoom_level_internal(self, steps[0], TRUE);
    }
}

void viewer_set_fit_to_window(Viewer *self, gboolean fit) {
    self->fit_to_window = fit ? TRUE : FALSE;
    if (fit) {
        self->fit_to_width = FALSE;
        viewer_update_image(self);
        /* center viewport */
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
        double page_x = gtk_adjustment_get_page_size(hadj);
        double page_y = gtk_adjustment_get_page_size(vadj);
        double upper_x = gtk_adjustment_get_upper(hadj) - page_x;
        double upper_y = gtk_adjustment_get_upper(vadj) - page_y;
        gtk_adjustment_set_value(hadj, CLAMP(upper_x/2.0, 0.0, upper_x));
        gtk_adjustment_set_value(vadj, CLAMP(upper_y/2.0, 0.0, upper_y));
    }
}

void viewer_set_fit_to_width(Viewer *self) {
    /* Fit image width to the *visible viewport* width, preserving aspect ratio.
     * We also preserve the current viewport center to avoid jumping to (0,0).
     */
    double before_scale = self->fit_to_window ? get_fit_zoom_level(self) : self->zoom_level;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));

    double page_x = gtk_adjustment_get_page_size(hadj);
    double page_y = gtk_adjustment_get_page_size(vadj);
    double val_x = gtk_adjustment_get_value(hadj);
    double val_y = gtk_adjustment_get_value(vadj);

    if (before_scale > 1e-6) {
        self->pending_center_x = (val_x + page_x / 2.0) / before_scale;
        self->pending_center_y = (val_y + page_y / 2.0) / before_scale;
        self->has_pending_center = TRUE;
        self->center_retry_count = 0;
    } else {
        self->has_pending_center = FALSE;
    }

    self->fit_to_window = FALSE;
    self->fit_to_width = TRUE;

    /* Compute zoom from current viewport width. If layout isn't ready yet,
     * viewer_update_image() will recompute once page-size becomes non-zero.
     */
    self->zoom_level = get_fit_width_zoom(self);
    viewer_update_image(self);
}

void viewer_zoom_reset(Viewer *self) {
    /* Reset to fit-to-window (consistent with earlier behavior) */
    viewer_set_fit_to_window(self, TRUE);
}

gboolean viewer_is_fit_to_width(Viewer *self)
{
    return self->fit_to_width;
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

