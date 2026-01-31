/* Helpers to capture and save video frames from the viewer.
 *
 * This file implements a small, synchronous capture path that reuses the
 * thumbnailing pipeline to extract a GdkPixbuf for the current video
 * position, then saves it using the viewer's existing image-save helpers.
 */

#include "viewer.h"
#include "thumbnails.h"
#include <gst/gst.h>

/* Capture current video frame (synchronous). Returns a new GdkPixbuf ref in
 * `out_pixbuf` (caller must unref). The requested width is a hint (0 = native).
 */
bool
viewer_capture_current_video_frame(Viewer *self, int width, GdkPixbuf **out_pixbuf, GError **error)
{
    if (!self || !self->playbin || !out_pixbuf) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid arguments");
        return FALSE;
    }

    if (!self->path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "no video path available");
        return FALSE;
    }

    /* Prefer capturing at displayed size if caller didn't request a width. */
    int req_w = width;
    if (req_w <= 0) {
        int aw = 0;
        if (GTK_IS_WIDGET(self->active_picture) && gtk_widget_get_realized(self->active_picture))
            aw = gtk_widget_get_width(self->active_picture);
        req_w = aw > 0 ? aw : 640;
    }

    return thumbnails_capture_video_frame(self->path, req_w, out_pixbuf, error);
}

/* Save the currently-displayed video frame to `dest_path` (png|jpeg). This
 * wraps capture + flattening + gdk_pixbuf_save and mirrors viewer_save_image
 * behaviour for parity with the image save flow. */
bool
viewer_save_current_video_frame(Viewer *self, const char *dest_path, const char *format, int quality, guint8 bg_r, guint8 bg_g, guint8 bg_b, GError **error)
{
    if (!self || !dest_path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid arguments");
        return FALSE;
    }

    GError *err = NULL;
    GdkPixbuf *pix = NULL;
    if (!viewer_capture_current_video_frame(self, 0, &pix, &err)) {
        g_propagate_error(error, err);
        return FALSE;
    }

    gboolean ok = FALSE;
    GdkPixbuf *to_save = pix;
    if (gdk_pixbuf_get_has_alpha(pix) && format && g_strcmp0(format, "jpeg") == 0) {
        to_save = flatten_alpha_to_rgb(pix, bg_r, bg_g, bg_b);
        if (!to_save) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "failed to flatten alpha");
            g_object_unref(pix);
            return FALSE;
        }
    } else {
        to_save = g_object_ref(pix);
    }

    if (format && g_strcmp0(format, "jpeg") == 0) {
        char qbuf[4];
        int q = quality;
        if (q <= 0 || q > 100) q = 85;
        g_snprintf(qbuf, sizeof(qbuf), "%d", q);
        ok = gdk_pixbuf_save(to_save, dest_path, "jpeg", &err, "quality", qbuf, NULL);
    } else {
        ok = gdk_pixbuf_save(to_save, dest_path, "png", &err, NULL);
    }

    g_object_unref(to_save);
    g_object_unref(pix);

    if (!ok) g_propagate_error(error, err);
    return ok;
}
