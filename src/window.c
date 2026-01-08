/* Window (application UI)
 *
 * Implements the main application window, wiring together the viewer,
 * thumbnails, metadata sidebar, and OCR interactions. Handles application
 * level UI actions, menus and window lifecycle.
 *
 * Sections: internal helpers, lifecycle (class_init/dispose), UI setup and
 * signal connection handlers.
 */

#include "window.h"
#include "viewer.h"
#include "curator.h"
#include "thumbnails.h"
#include "metadata.h"
#include "ocr.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

static void request_delete_current(BrightEyesWindow *self);
static void delete_current_now(BrightEyesWindow *self);
static void on_next_clicked(GtkButton *btn, BrightEyesWindow *self);

static void start_ocr_for_path(BrightEyesWindow *self, const char *path, const char *tmp_path);
struct _BrightEyesWindow {
    AdwApplicationWindow parent_instance;
    Viewer *viewer;
    Curator *curator;
    ThumbnailsBar *thumbnails;
    GtkHeaderBar *header_bar;
    AdwOverlaySplitView *split_view; /* Thumbnails (Outer) */
    AdwOverlaySplitView *metadata_view; /* Metadata (Inner) */
    GtkWidget *toast_overlay; /* Added toast overlay */
    GtkWidget *metadata_sidebar;
    guint slideshow_id;
    guint slideshow_duration;
    GtkWidget *slideshow_btn;
    GtkWidget *status_label;

    gboolean viewer_dark_background;
    gboolean confirm_delete;
    gboolean default_fit_to_window; /* TRUE=fit, FALSE=100% */
    
    /* Config */
    GAppInfo *selected_editor;
    GList *editor_candidates; /* List of GAppInfo* */

    /* OCR */
    char *ocr_language; /* Tesseract language code, e.g. "eng" */
};

G_DEFINE_TYPE(BrightEyesWindow, bright_eyes_window, ADW_TYPE_APPLICATION_WINDOW)

typedef struct {
    const char *code;
    const char *url;
    goffset min_bytes;
} BestLangInfo;

/* Tessdata best endpoints for the languages we expose. min_bytes is a sanity floor to catch truncated downloads. */
static const BestLangInfo best_langs[] = {
    { "eng", "https://github.com/tesseract-ocr/tessdata_best/raw/main/eng.traineddata", 5 * 1024 * 1024 },
    { "deu", "https://github.com/tesseract-ocr/tessdata_best/raw/main/deu.traineddata", 5 * 1024 * 1024 },
    { "fra", "https://github.com/tesseract-ocr/tessdata_best/raw/main/fra.traineddata", 5 * 1024 * 1024 },
    { "spa", "https://github.com/tesseract-ocr/tessdata_best/raw/main/spa.traineddata", 5 * 1024 * 1024 },
    { "ita", "https://github.com/tesseract-ocr/tessdata_best/raw/main/ita.traineddata", 5 * 1024 * 1024 },
    { "por", "https://github.com/tesseract-ocr/tessdata_best/raw/main/por.traineddata", 5 * 1024 * 1024 },
    { "jpn", "https://github.com/tesseract-ocr/tessdata_best/raw/main/jpn.traineddata", 5 * 1024 * 1024 },
    { "chi_sim", "https://github.com/tesseract-ocr/tessdata_best/raw/main/chi_sim.traineddata", 5 * 1024 * 1024 }
};

static const BestLangInfo *
find_best_info(const char *code)
{
    if (!code) return NULL;
    for (guint i = 0; i < G_N_ELEMENTS(best_langs); i++) {
        if (g_strcmp0(best_langs[i].code, code) == 0)
            return &best_langs[i];
    }
    return NULL;
}

static char *
tessdata_cache_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "brighteyes", "tessdata", NULL);
}

static gboolean
ensure_dir_exists(const char *path, GError **error)
{
    if (!path) return FALSE;
    if (g_mkdir_with_parents(path, 0755) == -1 && errno != EEXIST) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Failed to create %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static char *
traineddata_path_for(const char *cache_dir, const char *code)
{
    g_autofree char *name = g_strdup_printf("%s.traineddata", code);
    return g_build_filename(cache_dir, name, NULL);
}

static gboolean
traineddata_exists(const char *cache_dir, const char *code)
{
    g_autofree char *path = traineddata_path_for(cache_dir, code);
    return path && g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

static GPtrArray *
collect_missing_best(const char *lang, const char *cache_dir)
{
    GPtrArray *missing = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) parts = g_strsplit(lang ? lang : "eng", "+", -1);
    for (guint i = 0; parts && parts[i]; i++) {
        if (*parts[i] == '\0') continue;
        if (!traineddata_exists(cache_dir, parts[i]))
            g_ptr_array_add(missing, g_strdup(parts[i]));
    }
    return missing;
}

typedef struct {
    char *url;
    char *dest_path;
    goffset min_bytes;
} DownloadJob;

static void
download_job_free(DownloadJob *job)
{
    if (!job) return;
    g_free(job->url);
    g_free(job->dest_path);
    g_free(job);
}

static void
download_job_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    DownloadJob *job = task_data;
    GError *error = NULL;

    GFile *remote = g_file_new_for_uri(job->url);
    GFileInputStream *in = g_file_read(remote, cancellable, &error);
    if (!in) {
        g_object_unref(remote);
        g_task_return_error(task, error);
        return;
    }

    GFile *dest = g_file_new_for_path(job->dest_path);
    GFileOutputStream *out = g_file_replace(dest, NULL, FALSE, G_FILE_CREATE_NONE, cancellable, &error);
    if (!out) {
        g_object_unref(in);
        g_object_unref(dest);
        g_object_unref(remote);
        g_task_return_error(task, error);
        return;
    }

    guint8 chunk[8192];
    goffset total = 0;
    while (TRUE) {
        gssize n = g_input_stream_read(G_INPUT_STREAM(in), chunk, sizeof(chunk), cancellable, &error);
        if (n == 0) break;
        if (n < 0) {
            g_task_return_error(task, error);
            g_object_unref(out);
            g_object_unref(in);
            g_object_unref(dest);
            g_object_unref(remote);
            return;
        }
        if (!g_output_stream_write_all(G_OUTPUT_STREAM(out), chunk, (gsize)n, NULL, cancellable, &error)) {
            g_task_return_error(task, error);
            g_object_unref(out);
            g_object_unref(in);
            g_object_unref(dest);
            g_object_unref(remote);
            return;
        }
        total += n;
    }

    if (!g_output_stream_flush(G_OUTPUT_STREAM(out), cancellable, &error)) {
        g_task_return_error(task, error);
        g_object_unref(out);
        g_object_unref(in);
        g_object_unref(dest);
        g_object_unref(remote);
        return;
    }

    if (!g_output_stream_close(G_OUTPUT_STREAM(out), cancellable, &error)) {
        g_task_return_error(task, error);
        g_object_unref(out);
        g_object_unref(in);
        g_object_unref(dest);
        g_object_unref(remote);
        return;
    }

    if (job->min_bytes > 0 && total < job->min_bytes) {
        g_task_return_error(task, g_error_new(g_quark_from_static_string("download"), 1, "Downloaded file too small (%lld bytes)", (long long)total));
        g_object_unref(in);
        g_object_unref(dest);
        g_object_unref(remote);
        return;
    }

    g_object_unref(out);
    g_object_unref(in);
    g_object_unref(dest);
    g_object_unref(remote);
    g_task_return_boolean(task, TRUE);
}

static void
download_traineddata_async(const BestLangInfo *info, const char *cache_dir, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer user_data)
{
    g_autofree char *dest = traineddata_path_for(cache_dir, info->code);
    DownloadJob *job = g_new0(DownloadJob, 1);
    job->url = g_strdup(info->url);
    job->dest_path = g_strdup(dest);
    job->min_bytes = info->min_bytes;

    GTask *task = g_task_new(NULL, cancellable, cb, user_data);
    g_task_set_task_data(task, job, (GDestroyNotify)download_job_free);
    g_task_run_in_thread(task, download_job_thread);
    g_object_unref(task);
}

static gboolean
download_traineddata_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void
update_title(BrightEyesWindow *self)
{
    /* User requested application name only */
    gtk_window_set_title(GTK_WINDOW(self), "BrightEyes");
}

/* Public method to open a file */
void
load_image_path(BrightEyesWindow *self, const char *path)
{
    viewer_load_file(self->viewer, path);
    /* Apply preferred default zoom mode */
    if (self->default_fit_to_window)
        viewer_set_fit_to_window(self->viewer, TRUE);
    else
        viewer_zoom_reset(self->viewer);
    update_title(self);
    
    /* Update metadata whenever an image is loaded */
    metadata_sidebar_update(self->metadata_sidebar, path);
}

static void
on_viewer_zoom_changed(Viewer *viewer, guint percentage, BrightEyesWindow *self)
{
    char *text = g_strdup_printf("Zoom: %u%%", percentage);
    gtk_label_set_text(GTK_LABEL(self->status_label), text);
    g_free(text);
}

static void
on_zoom_in_clicked(GtkButton *btn, BrightEyesWindow *self) {
    viewer_zoom_in(self->viewer);
}

static void
on_zoom_out_clicked(GtkButton *btn, BrightEyesWindow *self) {
    viewer_zoom_out(self->viewer);
}

static void
on_fit_clicked(GtkButton *btn, BrightEyesWindow *self) {
    viewer_set_fit_to_window(self->viewer, TRUE);
}

static void
on_rotate_left_clicked(GtkButton *btn, BrightEyesWindow *self) {
    viewer_rotate_ccw(self->viewer);
}

static void
on_rotate_right_clicked(GtkButton *btn, BrightEyesWindow *self) {
    viewer_rotate_cw(self->viewer);
}

static gboolean
slideshow_tick(gpointer user_data) {
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    const char *next = curator_get_next(self->curator);
    if (!next) return FALSE;
    load_image_path(self, next);
    return TRUE;
}

static void
toggle_slideshow(BrightEyesWindow *self) {
    if (self->slideshow_id > 0) {
        g_source_remove(self->slideshow_id);
        self->slideshow_id = 0;
        gtk_button_set_icon_name(GTK_BUTTON(self->slideshow_btn), "media-playback-start-symbolic");
    } else {
        self->slideshow_id = g_timeout_add_seconds(self->slideshow_duration, slideshow_tick, self);
        gtk_button_set_icon_name(GTK_BUTTON(self->slideshow_btn), "media-playback-pause-symbolic");
        slideshow_tick(self);
    }
}

static void
on_thumbnail_activated(ThumbnailsBar *bar, const char *path, BrightEyesWindow *self) {
    if (path && self->curator) {
        curator_set_current_file(self->curator, path);
        const char *current = curator_get_current(self->curator);
        load_image_path(self, current);
    }
}

static void
on_folder_opened(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(dialog, result, &error);

    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            curator_load_directory(self->curator, path);
            const char *current = curator_get_current(self->curator);
            load_image_path(self, current);
            thumbnails_bar_refresh(self->thumbnails);
            /* Show sidebar when folder is loaded */
            adw_overlay_split_view_set_show_sidebar(self->split_view, TRUE);
            g_free(path);
        }
        g_object_unref(file);
    }
    if (error) g_error_free(error);
    g_object_unref(self);
}

static void
show_open_folder_dialog(BrightEyesWindow *self)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL, on_folder_opened, g_object_ref(self));
    g_object_unref(dialog);
}

/* Navigation Helpers for HUD */
static void
on_prev_clicked(GtkButton *btn, BrightEyesWindow *self) {
    const char *prev = curator_get_prev(self->curator);
    load_image_path(self, prev);
}

static void
on_next_clicked(GtkButton *btn, BrightEyesWindow *self) {
    const char *next = curator_get_next(self->curator);
    load_image_path(self, next);
}

/* on_key_pressed removed - consolidated into on_window_key_pressed */

static void
on_file_opened(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, &error);

    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
             bright_eyes_window_open_file(self, path);
             g_free(path);
        }
        g_object_unref(file);
    }
    if (error) g_error_free(error);
    g_object_unref(self);
}

static void
on_ocr_whole_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    const char *path = curator_get_current(self->curator);
    start_ocr_for_path(self, path, NULL);
}

static void
on_clear_selection_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    viewer_clear_selection(self->viewer);
}

static void
on_selection_cancel_response(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    if (g_strcmp0(response, "cancel") == 0) {
        viewer_set_selection_mode(self->viewer, FALSE);
        viewer_clear_selection(self->viewer);
    } else if (g_strcmp0(response, "ok") == 0) {
         /* Re-show toast if they want to keep drawing */
         AdwToast *toast = adw_toast_new("Selection Mode: Draw a box on the image.");
         adw_toast_set_timeout(toast, 0); /* Persist until action taken */
         adw_toast_set_button_label(toast, "Scan");
         adw_toast_set_action_name(toast, "win.ocr-selection");
         adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), toast);
    }
}

static void
on_ocr_selection_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);

    if (viewer_get_selection_mode(self->viewer)) {
        /* Already in selection mode. */
        if (!viewer_has_selection(self->viewer)) {
            /* Mode on, but no selection? Maybe user wants to cancel or is confused. */
             AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new("Selections", "Please draw a box on the image to select text."));
             adw_alert_dialog_add_response(dlg, "cancel", "Cancel Mode");
             adw_alert_dialog_add_response(dlg, "ok", "Keep Drawing");
             adw_alert_dialog_set_default_response(dlg, "ok");
             adw_alert_dialog_set_close_response(dlg, "ok");
             g_signal_connect(dlg, "response", G_CALLBACK(on_selection_cancel_response), self);
             adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(self));
             return;
        }
    } else {
         /* Turn on selection mode */
         viewer_set_selection_mode(self->viewer, TRUE);
         
         /* Show a toast to inform the user with a Scan button */
         AdwToast *toast = adw_toast_new("Selection Mode: Draw a box on the image.");
         adw_toast_set_timeout(toast, 0); /* Persist until action taken */
         adw_toast_set_button_label(toast, "Scan");
         adw_toast_set_action_name(toast, "win.ocr-selection");
         adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), toast);
         return; /* Wait for user to draw and click Scan */
    }

    /* If we got here, we are in selection mode AND have a selection. Proceed to OCR. */

    GdkPixbuf *sel = viewer_get_selection_pixbuf(self->viewer);
    if (!sel) {
        AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new("Selection Error", "Failed to extract the selected region."));
        adw_alert_dialog_add_response(dlg, "ok", "OK");
        adw_alert_dialog_set_close_response(dlg, "ok");
        adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(self));
        return;
    }

    /* Create temp file */
    char *tmpl = g_strdup_printf("%s/brighteyes-ocr-XXXXXX", g_get_tmp_dir());
    int fd = mkstemp(tmpl);
    if (fd == -1) {
        g_warning("Failed to create temp file %s: %s", tmpl, g_strerror(errno));
        g_free(tmpl);
        g_object_unref(sel);
        return;
    }
    close(fd);

    GError *err = NULL;
    if (!gdk_pixbuf_save(sel, tmpl, "png", &err, NULL)) {
        g_warning("Failed to save selection to %s: %s", tmpl, err ? err->message : "unknown");
        if (err) g_clear_error(&err);
        unlink(tmpl);
        g_free(tmpl);
        g_object_unref(sel);
        return;
    }

    g_object_unref(sel);

    /* Start OCR flow using temporary file; it will be removed after OCR completes */
    start_ocr_for_path(self, tmpl, tmpl);
    g_free(tmpl);
    
    /* Auto-clear selection and disable mode after triggering scan */
    viewer_clear_selection(self->viewer);
    viewer_set_selection_mode(self->viewer, FALSE);
}
static void
show_open_dialog(BrightEyesWindow *self)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Image");
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_file_opened, g_object_ref(self));
    g_object_unref(dialog);
}

static void
toggle_sidebar(BrightEyesWindow *self)
{
     gboolean show = adw_overlay_split_view_get_show_sidebar(self->split_view);
     adw_overlay_split_view_set_show_sidebar(self->split_view, !show);
}

static void
toggle_metadata(BrightEyesWindow *self)
{
     gboolean show = adw_overlay_split_view_get_show_sidebar(self->metadata_view);
     adw_overlay_split_view_set_show_sidebar(self->metadata_view, !show);
}

typedef struct { BrightEyesWindow *win; GtkWindow *dlg; char *text; char *tmp_path; } OCRCallbackData;

typedef struct {
    GtkWindow *window;
    GtkWidget *label;
} SpinnerWindow;

static SpinnerWindow
create_spinner_window(BrightEyesWindow *self, const char *title, const char *message)
{
    SpinnerWindow ui = {0};
    ui.window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_transient_for(ui.window, GTK_WINDOW(self));
    gtk_window_set_modal(ui.window, TRUE);
    gtk_window_set_title(ui.window, title);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);

    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_visible(spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(content), spinner);

    ui.label = gtk_label_new(message);
    gtk_box_append(GTK_BOX(content), ui.label);

    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);

    gtk_window_set_child(ui.window, content);
    return ui;
}

static void begin_ocr_async(BrightEyesWindow *self, const char *path, const char *lang, const char *datapath, const char *tmp_path);
static void start_ocr_for_path(BrightEyesWindow *self, const char *path, const char *tmp_path);

static void show_ocr_result_dialog(BrightEyesWindow *w, const char *text);
static void ocr_done_cb(GObject *source, GAsyncResult *res, gpointer user_data);

static void
begin_ocr_async(BrightEyesWindow *self, const char *path, const char *lang, const char *datapath, const char *tmp_path)
{
    SpinnerWindow ui = create_spinner_window(self, "Recognizing...", "Performing OCR...");

    g_object_ref(self); /* keep alive for callback */

    OCRCallbackData *cbdata = g_new0(OCRCallbackData, 1);
    cbdata->win = self;
    cbdata->dlg = ui.window;
    cbdata->tmp_path = tmp_path ? g_strdup(tmp_path) : NULL;

    const char *effective_lang = lang ? lang : "eng";
    ocr_recognize_image_async(path, effective_lang, datapath, NULL, ocr_done_cb, cbdata);

    gtk_window_present(ui.window);
}

typedef struct {
    BrightEyesWindow *win;
    char *path;
    char *lang;
    char *cache_dir;
    GPtrArray *missing;
    guint index;
    SpinnerWindow ui;
} DownloadFlow;

static void download_flow_free(DownloadFlow *ctx);
static void download_next_lang(DownloadFlow *ctx);

static void
download_lang_finished(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DownloadFlow *ctx = user_data;
    GError *err = NULL;

    if (!download_traineddata_finish(res, &err)) {
        g_warning("Download failed: %s", err ? err->message : "unknown error");
        if (ctx->ui.window)
            gtk_window_destroy(ctx->ui.window);
        begin_ocr_async(ctx->win, ctx->path, ctx->lang, NULL, NULL); /* fallback to lite */
        download_flow_free(ctx);
        g_clear_error(&err);
        return;
    }

    ctx->index++;
    if (ctx->index < ctx->missing->len) {
        download_next_lang(ctx);
        return;
    }

    if (ctx->ui.window)
        gtk_window_destroy(ctx->ui.window);

    begin_ocr_async(ctx->win, ctx->path, ctx->lang, ctx->cache_dir, NULL);
    download_flow_free(ctx);
}

static void
download_next_lang(DownloadFlow *ctx)
{
    if (ctx->index >= ctx->missing->len) {
        begin_ocr_async(ctx->win, ctx->path, ctx->lang, ctx->cache_dir, NULL);
        download_flow_free(ctx);
        return;
    }

    const char *code = g_ptr_array_index(ctx->missing, ctx->index);
    const BestLangInfo *info = find_best_info(code);
    if (!info) {
        g_warning("No download source for %s; using lite model", code);
        if (ctx->ui.window)
            gtk_window_destroy(ctx->ui.window);
        begin_ocr_async(ctx->win, ctx->path, ctx->lang, NULL, NULL);
        download_flow_free(ctx);
        return;
    }

    if (ctx->ui.label) {
        g_autofree char *msg = g_strdup_printf("Downloading %s (full accuracy)...", code);
        gtk_label_set_text(GTK_LABEL(ctx->ui.label), msg);
    }

    download_traineddata_async(info, ctx->cache_dir, NULL, download_lang_finished, ctx);
}

static void
download_flow_free(DownloadFlow *ctx)
{
    if (!ctx) return;
    g_clear_object(&ctx->win);
    g_free(ctx->path);
    g_free(ctx->lang);
    g_free(ctx->cache_dir);
    if (ctx->missing)
        g_ptr_array_free(ctx->missing, TRUE);
    g_free(ctx);
}

static void
start_best_download(BrightEyesWindow *self, const char *path, const char *lang, const char *cache_dir, GPtrArray *missing)
{
    DownloadFlow *ctx = g_new0(DownloadFlow, 1);
    ctx->win = g_object_ref(self);
    ctx->path = g_strdup(path);
    ctx->lang = g_strdup(lang ? lang : "eng");
    ctx->cache_dir = g_strdup(cache_dir);
    ctx->missing = missing; /* take ownership */
    ctx->index = 0;
    ctx->ui = create_spinner_window(self, "Downloading OCR data", "Preparing download...");
    gtk_window_present(ctx->ui.window);
    download_next_lang(ctx);
}

typedef struct {
    BrightEyesWindow *win;
    char *path;
    char *lang;
    char *cache_dir;
    GPtrArray *missing;
} OcrRequest;

static void
ocr_request_free(OcrRequest *req)
{
    if (!req) return;
    g_clear_object(&req->win);
    g_free(req->path);
    g_free(req->lang);
    g_free(req->cache_dir);
    if (req->missing)
        g_ptr_array_free(req->missing, TRUE);
    g_free(req);
}

static void
on_best_choice(AdwMessageDialog *dialog, const char *response, gpointer user_data)
{
    OcrRequest *req = user_data;

    if (g_strcmp0(response, "download") == 0) {
        start_best_download(req->win, req->path, req->lang, req->cache_dir, req->missing);
        req->missing = NULL; /* ownership transferred */
    } else {
        begin_ocr_async(req->win, req->path, req->lang, NULL, NULL);
    }

    ocr_request_free(req);
}

static void start_ocr_for_path(BrightEyesWindow *self, const char *path, const char *tmp_path)
{
    if (!path) return;

    const char *lang = self->ocr_language ? self->ocr_language : "eng";
    g_autofree char *cache_dir = tessdata_cache_dir();
    if (!cache_dir) {
        begin_ocr_async(self, path, lang, NULL, tmp_path);
        return;
    }

    GError *dir_err = NULL;
    if (!ensure_dir_exists(cache_dir, &dir_err)) {
        g_warning("Cannot prepare cache dir: %s", dir_err ? dir_err->message : "unknown error");
        g_clear_error(&dir_err);
        begin_ocr_async(self, path, lang, NULL, tmp_path);
        return;
    }

    GPtrArray *missing = collect_missing_best(lang, cache_dir);
    if (!missing || missing->len == 0) {
        if (missing)
            g_ptr_array_free(missing, TRUE);
        begin_ocr_async(self, path, lang, cache_dir, tmp_path);
        return;
    }

    /* Build comma-separated missing list for the dialog body */
    g_auto(GStrv) missing_vec = g_new0(char *, missing->len + 1);
    for (guint i = 0; i < missing->len; i++)
        missing_vec[i] = g_strdup(g_ptr_array_index(missing, i));
    g_autofree char *missing_csv = g_strjoinv(", ", missing_vec);
    g_autofree char *body = g_strdup_printf("Download full-accuracy data for %s (~20-25 MB each)? The lite models stay bundled for quick results.", missing_csv ? missing_csv : "selected languages");

    OcrRequest *req = g_new0(OcrRequest, 1);
    req->win = g_object_ref(self);
    req->path = g_strdup(path);
    req->lang = g_strdup(lang);
    req->cache_dir = g_strdup(cache_dir);
    req->missing = missing; /* ownership moved */

    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new("Full accuracy download", body));
    adw_alert_dialog_add_responses(dlg,
                                   "lite", "Use lite for now",
                                   "download", "Download full data",
                                   NULL);
    adw_alert_dialog_set_default_response(dlg, "download");
    adw_alert_dialog_set_close_response(dlg, "lite");
    g_signal_connect(dlg, "response", G_CALLBACK(on_best_choice), req);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(self));
}

static void
ocr_done_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    OCRCallbackData *d = user_data;
    BrightEyesWindow *w = d->win;
    GtkWindow *progress = d->dlg;

    GError *err = NULL;
    char *text = ocr_recognize_image_finish(res, &err);

    if (progress)
        gtk_window_destroy(progress);

    /* Defensive checks: ensure parent window is still a valid GtkWindow */
    if (!GTK_IS_WINDOW(w)) {
        g_warning("Parent window gone; dropping OCR result");
        g_free(text);
        g_free(d);
        g_object_unref(w);
        return;
    }

    if (err) {
        /* Use a simple transient message window; avoid deprecated GtkMessageDialog casting issues */
        GtkWindow *msg = GTK_WINDOW(gtk_window_new());
        gtk_window_set_transient_for(msg, GTK_WINDOW(w));
        gtk_window_set_modal(msg, TRUE);
        gtk_window_set_title(msg, "OCR Error");
        GtkWidget *lbl = gtk_label_new(err->message);
        gtk_window_set_child(msg, lbl);
        gtk_window_present(msg);
        g_clear_error(&err);
    } else {
        show_ocr_result_dialog(w, text ? text : "");
    }

    /* If this OCR was run on a temporary file (e.g., selection), remove it now */
    if (d->tmp_path) {
        unlink(d->tmp_path);
        g_free(d->tmp_path);
    }

    g_free(text);
    g_free(d);
    g_object_unref(w);
}

static void
copy_text_cb(GtkWidget *btn, gpointer user_data)
{
    const char *txt = user_data;
    GdkDisplay *display = gdk_display_get_default();
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_set_text(clipboard, txt);
}

static void
open_text_cb(GtkWidget *btn, gpointer user_data)
{
    const char *txt = user_data;
    /* Use template that ends with XXXXXX (required by mkstemp) */
    char *tmpl = g_strdup_printf("%s/brighteyes-ocr-XXXXXX", g_get_tmp_dir());
    int fd = mkstemp(tmpl);
    if (fd == -1) {
        g_warning("Failed to create temp file %s: %s", tmpl, g_strerror(errno));
        g_free(tmpl);
        return;
    }
    /* Ensure text is valid UTF-8; if not, make it valid to avoid editor encoding errors */
    gboolean is_valid = g_utf8_validate(txt, -1, NULL);
    char *write_buf = NULL;
    if (is_valid) {
        write_buf = g_strdup(txt);
    } else {
        g_warning("OCR text contained invalid UTF-8; replacing invalid sequences");
        write_buf = g_utf8_make_valid(txt, -1);
    }

    /* Write content */
    ssize_t len_total = 0;
    const char *p = write_buf;
    ssize_t to_write = strlen(write_buf);
    while (to_write > 0) {
        ssize_t written = write(fd, p, to_write);
        if (written == -1) {
            g_warning("Failed to write to temp file %s: %s", tmpl, g_strerror(errno));
            close(fd);
            unlink(tmpl);
            g_free(tmpl);
            g_free(write_buf);
            return;
        }
        to_write -= written;
        p += written;
        len_total += written;
    }
    /* Ensure contents reach disk */
    if (fsync(fd) == -1) {
        g_warning("fsync failed for %s: %s", tmpl, g_strerror(errno));
    }
    /* Set secure permissions in case umask is permissive */
    fchmod(fd, S_IRUSR | S_IWUSR);
    close(fd);
    g_free(write_buf);

    /* Check file size to ensure data was written */
    struct stat st;
    if (stat(tmpl, &st) == 0) {
        g_debug("Temp file %s size=%lld", tmpl, (long long)st.st_size);
        if (st.st_size == 0) {
            g_warning("Temp file %s was written with 0 bytes", tmpl);
        }
    } else {
        g_warning("Failed to stat %s: %s", tmpl, g_strerror(errno));
    }

    /* Optionally add .txt extension by renaming the file; if rename fails we just continue with the no-ext file */
    char *with_ext = g_strdup_printf("%s.txt", tmpl);
    if (rename(tmpl, with_ext) == 0) {
        /* success, use new path */
        g_free(tmpl);
        tmpl = with_ext;
    } else {
        /* failed to rename; keep original temporary filename */
        g_free(with_ext);
    }

    /* Re-stat after rename */
    if (stat(tmpl, &st) == 0) {
        g_debug("Using temp file %s size=%lld", tmpl, (long long)st.st_size);
    }

    char *uri = g_filename_to_uri(tmpl, NULL, NULL);
    if (uri) {
        GError *err = NULL;
        if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
            g_warning("Failed to launch default app for %s: %s", uri, err ? err->message : "");
            g_clear_error(&err);
        }
        g_free(uri);
    }

    /* Don't remove file immediately; user may want to save it from editor */
    g_free(tmpl);
}

static void
show_ocr_result_dialog(BrightEyesWindow *w, const char *text)
{
    /* Use a top-level GtkWindow so resizing and content expansion work reliably */
    GtkWindow *rdlg = GTK_WINDOW(gtk_window_new());
    gtk_window_set_transient_for(rdlg, GTK_WINDOW(w));
    gtk_window_set_modal(rdlg, TRUE);
    gtk_window_set_title(rdlg, "OCR Result");
    gtk_window_set_default_size(rdlg, 700, 500);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *sc = gtk_scrolled_window_new();
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), text, -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), sc);

    /* Ensure the scrolled area and text view expand and resize with window */
    gtk_widget_set_hexpand(sc, TRUE);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_widget_set_hexpand(tv, TRUE);
    gtk_widget_set_vexpand(tv, TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    /* Buttons box anchored to lower-right */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *copy_btn = gtk_button_new_from_icon_name("edit-copy");
    gtk_widget_set_tooltip_text(copy_btn, "Copy to clipboard");
    GtkWidget *open_btn = gtk_button_new_from_icon_name("accessories-text-editor");
    gtk_widget_set_tooltip_text(open_btn, "Open in text editor");
    gtk_box_append(GTK_BOX(btn_box), copy_btn);
    gtk_box_append(GTK_BOX(btn_box), open_btn);

    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_valign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_end(btn_box, 8);
    gtk_widget_set_margin_bottom(btn_box, 8);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), btn_box);

    /* Attach overlay as window child */
    gtk_window_set_child(rdlg, overlay);

    /* Connect copy and open using helper callbacks */
    char *dup_text = g_strdup(text);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(copy_text_cb), dup_text);
    g_signal_connect(open_btn, "clicked", G_CALLBACK(open_text_cb), dup_text);
    /* Free duplicated text when window is destroyed */
    g_object_set_data_full(G_OBJECT(rdlg), "ocr-text", dup_text, g_free);

    gtk_window_present(rdlg);
}

static void
apply_viewer_background(BrightEyesWindow *self)
{
    if (self->viewer)
        viewer_set_dark_background(self->viewer, self->viewer_dark_background);
}

static void
apply_default_zoom_pref(BrightEyesWindow *self)
{
    if (self->viewer)
        viewer_set_default_fit(self->viewer, self->default_fit_to_window);
}

static void
delete_current_now(BrightEyesWindow *self)
{
    const char *current = curator_get_current(self->curator);
    if (!current) return;

    GError *err = NULL;
    if (!curator_trash_current(self->curator, &err)) {
        g_warning("Failed to move to trash: %s", err ? err->message : "unknown");
        g_clear_error(&err);
        return;
    }

    const char *next = curator_get_current(self->curator);
    load_image_path(self, next);
    thumbnails_bar_refresh(self->thumbnails);
}

static void
on_delete_confirm_response(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    if (g_strcmp0(response, "trash") == 0) {
        delete_current_now(self);
    }
}

static void
request_delete_current(BrightEyesWindow *self)
{
    const char *current = curator_get_current(self->curator);
    if (!current) return;

    if (!self->confirm_delete) {
        delete_current_now(self);
        return;
    }

    g_autofree char *name = g_path_get_basename(current);
    g_autofree char *body = g_strdup_printf("Move %s to trash?", name ? name : "this file");

    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new("Move to Trash?", body));
    adw_alert_dialog_add_response(dlg, "cancel", "Cancel");
    adw_alert_dialog_add_response(dlg, "trash", "Move to Trash");
    adw_alert_dialog_set_response_appearance(dlg, "trash", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dlg, "cancel");
    adw_alert_dialog_set_close_response(dlg, "cancel");
    
    g_signal_connect(dlg, "response", G_CALLBACK(on_delete_confirm_response), self);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(self));
}

/* Renamed from on_window_key_pressed to match call site in init */
static gboolean
on_window_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    
    if (keyval == GDK_KEY_Delete) {
        request_delete_current(self);
        return TRUE;
    }

    /* Original Shortcuts */
     switch (keyval) {
        case GDK_KEY_Right:
        case GDK_KEY_space:
        {
            const char *next = curator_get_next(self->curator);
            load_image_path(self, next);
            return TRUE;
        }
        case GDK_KEY_Left:
        {
            const char *prev = curator_get_prev(self->curator);
            load_image_path(self, prev);
            return TRUE;
        }
        case GDK_KEY_0:
        case GDK_KEY_KP_0:
             if (state & GDK_CONTROL_MASK) viewer_zoom_reset(self->viewer);
             return TRUE;
        case GDK_KEY_r:
             if (state & GDK_CONTROL_MASK) viewer_rotate_cw(self->viewer);
             return TRUE;
        case GDK_KEY_l:
             if (state & GDK_CONTROL_MASK) viewer_rotate_ccw(self->viewer);
             return TRUE;
        case GDK_KEY_F11:
        {
             if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
                gtk_window_unfullscreen(GTK_WINDOW(self));
             else
                gtk_window_fullscreen(GTK_WINDOW(self));
            return TRUE;
        }
        case GDK_KEY_F9:
        {
            /* Toggle sidebar */
            toggle_metadata(self);
            return TRUE;
        }
    }
    
    return FALSE;
}


static void
on_editor_changed(AdwComboRow *combo, GParamSpec *pspec, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    guint selected = adw_combo_row_get_selected(combo);
    
    if (self->editor_candidates == NULL) return;
    
    GAppInfo *app = g_list_nth_data(self->editor_candidates, selected);
    if (app) {
        if (self->selected_editor) g_object_unref(self->selected_editor);
        self->selected_editor = g_object_ref(app);
    }
}

static void
on_open_editor_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    const char *current_file = curator_get_current(self->curator);
    
    if (!current_file) return;
    
    GFile *file = g_file_new_for_path(current_file);
    GList *files = g_list_append(NULL, file);
    GError *error = NULL;
    
    if (self->selected_editor) {
        g_app_info_launch(self->selected_editor, files, NULL, &error);
    } else {
        /* Default handler usually is the app itself for images, 
           so we might want to check specific helpers if no editor selected, 
           or just rely on the OS 'open' which might be us. 
           But usually g_app_info_launch_default_for_uri opens the default handler. */
        
        /* If we are the default, this might loop. 
           But let's assume the user wants the system default if they haven't picked one. */
        char *uri = g_file_get_uri(file);
        g_app_info_launch_default_for_uri(uri, NULL, &error);
        g_free(uri);
    }
    
    if (error) {
        g_warning("Failed to launch editor: %s", error->message);
        g_clear_error(&error);
    }
    
    g_list_free(files);
    g_object_unref(file);
}

static char *
get_config_path(void)
{
    const char *config_dir = g_get_user_config_dir();
    char *dir = g_build_filename(config_dir, "brighteyes", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "config.ini", NULL);
    g_free(dir);
    return path;
}

static void
save_settings(BrightEyesWindow *self)
{
    GKeyFile *key_file = g_key_file_new();
    
    g_key_file_set_integer(key_file, "Settings", "slideshow_duration", self->slideshow_duration);
    g_key_file_set_boolean(key_file, "Settings", "viewer_dark_background", self->viewer_dark_background);
    g_key_file_set_boolean(key_file, "Settings", "confirm_delete", self->confirm_delete);
    g_key_file_set_boolean(key_file, "Settings", "default_fit_to_window", self->default_fit_to_window);
    if (self->ocr_language) {
        g_key_file_set_string(key_file, "Settings", "ocr_language", self->ocr_language);
    }

    /* Save Dark Mode config */
    AdwStyleManager *manager = adw_style_manager_get_default();
    gboolean is_dark = adw_style_manager_get_dark(manager);
    g_key_file_set_boolean(key_file, "Settings", "dark_mode", is_dark);

    char *path = get_config_path();
    GError *error = NULL;
    if (!g_key_file_save_to_file(key_file, path, &error)) {
        g_warning("Failed to save settings: %s", error->message);
        g_error_free(error);
    }
    g_free(path);
    g_key_file_free(key_file);
}

static void
load_settings(BrightEyesWindow *self)
{
    char *path = get_config_path();
    GKeyFile *key_file = g_key_file_new();
    GError *error = NULL;
    
    if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
        if (g_key_file_has_key(key_file, "Settings", "slideshow_duration", NULL))
            self->slideshow_duration = (guint)g_key_file_get_integer(key_file, "Settings", "slideshow_duration", NULL);
            
        if (g_key_file_has_key(key_file, "Settings", "viewer_dark_background", NULL)) {
            self->viewer_dark_background = g_key_file_get_boolean(key_file, "Settings", "viewer_dark_background", NULL);
            apply_viewer_background(self);
        }
            
        if (g_key_file_has_key(key_file, "Settings", "confirm_delete", NULL))
            self->confirm_delete = g_key_file_get_boolean(key_file, "Settings", "confirm_delete", NULL);

        if (g_key_file_has_key(key_file, "Settings", "default_fit_to_window", NULL)) {
            self->default_fit_to_window = g_key_file_get_boolean(key_file, "Settings", "default_fit_to_window", NULL);
            apply_default_zoom_pref(self);
        }

        if (g_key_file_has_key(key_file, "Settings", "ocr_language", NULL)) {
            g_free(self->ocr_language);
            self->ocr_language = g_key_file_get_string(key_file, "Settings", "ocr_language", NULL);
        }

        if (g_key_file_has_key(key_file, "Settings", "dark_mode", NULL)) {
            gboolean is_dark = g_key_file_get_boolean(key_file, "Settings", "dark_mode", NULL);
            AdwStyleManager *manager = adw_style_manager_get_default();
            adw_style_manager_set_color_scheme(manager, is_dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
        }
    } else {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
             g_warning("Failed to load settings: %s", error->message);
        }
        g_clear_error(&error);
    }
    
    g_free(path);
    g_key_file_free(key_file);
}

static void
on_dark_mode_switched(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    AdwStyleManager *manager = adw_style_manager_get_default();
    gboolean is_dark = gtk_switch_get_active(sw);
    adw_style_manager_set_color_scheme(manager, is_dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    save_settings(self);
}

static void
on_dark_background_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    self->viewer_dark_background = gtk_switch_get_active(sw);
    apply_viewer_background(self);
    save_settings(self);
}

static void
on_confirm_delete_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    self->confirm_delete = gtk_switch_get_active(sw);
    save_settings(self);
}

static void
on_default_zoom_changed(AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    guint selected = adw_combo_row_get_selected(row);
    self->default_fit_to_window = (selected == 0);
    apply_default_zoom_pref(self);
    save_settings(self);
}

static void
on_duration_changed(GtkAdjustment *adj, GParamSpec *pspec, BrightEyesWindow *self)
{
    self->slideshow_duration = (guint)gtk_adjustment_get_value(adj);
    save_settings(self);
}

static void
on_ocr_language_changed(AdwComboRow *row, GParamSpec *pspec, BrightEyesWindow *self)
{
    GtkStringList *list = g_object_get_data(G_OBJECT(row), "lang-model");
    guint idx = adw_combo_row_get_selected(row);
    if (!list || idx == GTK_INVALID_LIST_POSITION)
        return;

    const char *lang = gtk_string_list_get_string(list, idx);
    if (!lang)
        return;

    g_free(self->ocr_language);
    self->ocr_language = g_strdup(lang);
    save_settings(self);
}

static void
on_preferences_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    
    AdwPreferencesDialog *pref_win = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    
    /* Page 1: General (Appearance & Files) */
    AdwPreferencesPage *page_general = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page_general, "General");
    adw_preferences_page_set_icon_name(page_general, "preferences-system-symbolic");

    /* Appearance group */
    AdwPreferencesGroup *appearance_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(appearance_group, "Appearance");

    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Dark Mode");
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    AdwStyleManager *manager = adw_style_manager_get_default();
    gboolean is_dark = adw_style_manager_get_dark(manager);
    gtk_switch_set_active(GTK_SWITCH(sw), is_dark);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_dark_mode_switched), self);
    adw_action_row_add_suffix(row, sw);
    adw_preferences_group_add(appearance_group, GTK_WIDGET(row));


    /* Use Dark Background */
    AdwActionRow *bg_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(bg_row), "Use Dark Background");
    adw_action_row_set_subtitle(bg_row, "Toggle between dark and light background for images"); 
    GtkWidget *bg_switch = gtk_switch_new();
    gtk_widget_set_valign(bg_switch, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(bg_switch), self->viewer_dark_background);
    g_signal_connect(bg_switch, "notify::active", G_CALLBACK(on_dark_background_toggled), self);
    adw_action_row_add_suffix(bg_row, bg_switch);
    adw_preferences_group_add(appearance_group, GTK_WIDGET(bg_row));

    adw_preferences_page_add(page_general, ADW_PREFERENCES_GROUP(appearance_group));

    /* Files group */
    AdwPreferencesGroup *files_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(files_group, "Files");

    AdwComboRow *combo = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(combo), "External Editor");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(combo), "Application used for 'Open in Editor'");

    if (self->editor_candidates) {
        g_list_free_full(self->editor_candidates, g_object_unref);
    }
    self->editor_candidates = g_app_info_get_all_for_type("image/jpeg");

    if (self->editor_candidates) {
        GtkStringList *list = gtk_string_list_new(NULL);
        guint selected_idx = 0;
        guint i = 0;

        GList *l;
        for (l = self->editor_candidates; l != NULL; l = l->next) {
             GAppInfo *app = G_APP_INFO(l->data);
             gtk_string_list_append(list, g_app_info_get_name(app));

             if (self->selected_editor && g_app_info_equal(app, self->selected_editor)) {
                 selected_idx = i;
             }
             i++;
        }

        adw_combo_row_set_model(combo, G_LIST_MODEL(list));
        adw_combo_row_set_selected(combo, selected_idx);
        g_signal_connect(combo, "notify::selected", G_CALLBACK(on_editor_changed), self);
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(combo), "No likely editors found");
    }

    adw_preferences_group_add(files_group, GTK_WIDGET(combo));

    AdwActionRow *confirm_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(confirm_row), "Ask Before Deleting");
    GtkWidget *confirm_switch = gtk_switch_new();
    gtk_widget_set_valign(confirm_switch, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(confirm_switch), self->confirm_delete);
    g_signal_connect(confirm_switch, "notify::active", G_CALLBACK(on_confirm_delete_toggled), self);
    adw_action_row_add_suffix(confirm_row, confirm_switch);
    adw_preferences_group_add(files_group, GTK_WIDGET(confirm_row));
    
    adw_preferences_page_add(page_general, ADW_PREFERENCES_GROUP(files_group));


    /* Page 2: Viewer */
    AdwPreferencesPage *page_viewer = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page_viewer, "Viewer");
    adw_preferences_page_set_icon_name(page_viewer, "image-x-generic-symbolic");

    /* Viewer group */
    AdwPreferencesGroup *viewer_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(viewer_group, "Viewer Settings");

    /* Default Zoom */
    GtkStringList *zoom_options = gtk_string_list_new(NULL);
    gtk_string_list_append(zoom_options, "Fit to window");
    gtk_string_list_append(zoom_options, "100% (1:1)");

    AdwComboRow *zoom_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(zoom_row), "Default Zoom");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(zoom_row), "Applied when opening items");
    adw_combo_row_set_model(zoom_row, G_LIST_MODEL(zoom_options));
    adw_combo_row_set_selected(zoom_row, self->default_fit_to_window ? 0 : 1);
    g_signal_connect(zoom_row, "notify::selected", G_CALLBACK(on_default_zoom_changed), self);
    adw_preferences_group_add(viewer_group, GTK_WIDGET(zoom_row));
    g_object_unref(zoom_options);

    /* Slideshow */
    AdwSpinRow *spin_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1.0, 60.0, 1.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin_row), "Slideshow Interval (seconds)");
    adw_spin_row_set_value(spin_row, (double)self->slideshow_duration);
    GtkAdjustment *adj = adw_spin_row_get_adjustment(spin_row);
    g_signal_connect(adj, "notify::value", G_CALLBACK(on_duration_changed), self);
    adw_preferences_group_add(viewer_group, GTK_WIDGET(spin_row));
    
    adw_preferences_page_add(page_viewer, ADW_PREFERENCES_GROUP(viewer_group));


    /* Page 3: OCR */
    AdwPreferencesPage *page_ocr = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page_ocr, "Text Recognition");
    adw_preferences_page_set_icon_name(page_ocr, "edit-find-symbolic");

    /* OCR group */
    AdwPreferencesGroup *ocr_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(ocr_group, "OCR Engine");

    static const char *languages[] = {
        "eng", "eng+deu", "eng+fra", "deu", "fra", "spa", "ita", "por", "jpn", "chi_sim"
    };
    GtkStringList *lang_list = gtk_string_list_new(NULL);
    guint selected_lang = 0;
    for (guint i = 0; i < G_N_ELEMENTS(languages); i++) {
        gtk_string_list_append(lang_list, languages[i]);
        if (self->ocr_language && g_strcmp0(self->ocr_language, languages[i]) == 0)
            selected_lang = i;
    }

    AdwComboRow *lang_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lang_row), "OCR Language");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(lang_row), "Tesseract language code");
    adw_combo_row_set_model(lang_row, G_LIST_MODEL(lang_list));
    adw_combo_row_set_selected(lang_row, selected_lang);
    g_object_set_data_full(G_OBJECT(lang_row), "lang-model", g_object_ref(lang_list), g_object_unref);
    g_signal_connect(lang_row, "notify::selected", G_CALLBACK(on_ocr_language_changed), self);
    adw_preferences_group_add(ocr_group, GTK_WIDGET(lang_row));
    g_object_unref(lang_list);

    adw_preferences_page_add(page_ocr, ADW_PREFERENCES_GROUP(ocr_group));

    adw_preferences_dialog_add(pref_win, page_general);
    adw_preferences_dialog_add(pref_win, page_viewer);
    adw_preferences_dialog_add(pref_win, page_ocr);
    
    adw_dialog_present(ADW_DIALOG(pref_win), GTK_WIDGET(self));
}


static void
on_shortcuts_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    
    const char *ui_data = 
    "<interface>"
    "  <object class='GtkShortcutsWindow' id='shortcuts_window'>"
    "    <property name='modal'>1</property>"
    "    <child>"
    "      <object class='GtkShortcutsSection'>"
    "        <property name='section-name'>shortcuts</property>"
    "        <property name='max-height'>10</property>"
    "        <child>"
    "          <object class='GtkShortcutsGroup'>"
    "            <property name='title'>Navigation</property>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Next Image</property>"
    "                <property name='accelerator'>Right</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Previous Image</property>"
    "                <property name='accelerator'>Left</property>"
    "              </object>"
    "            </child>"
    "          </object>"
    "        </child>"
    "        <child>"
    "          <object class='GtkShortcutsGroup'>"
    "            <property name='title'>View</property>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Zoom In</property>"
    "                <property name='accelerator'>plus</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Zoom Out</property>"
    "                <property name='accelerator'>minus</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Reset Zoom</property>"
    "                <property name='accelerator'>&lt;Ctrl&gt;0</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Rotate Right</property>"
    "                <property name='accelerator'>&lt;Ctrl&gt;r</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Rotate Left</property>"
    "                <property name='accelerator'>&lt;Ctrl&gt;l</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Fullscreen</property>"
    "                <property name='accelerator'>F11</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='title'>Toggle Sidebar</property>"
    "                <property name='accelerator'>F9</property>"
    "              </object>"
    "            </child>"
    "          </object>"
    "        </child>"
    "      </object>"
    "    </child>"
    "  </object>"
    "</interface>";

    GtkBuilder *builder = gtk_builder_new_from_string(ui_data, -1);
    GtkWidget *win = GTK_WIDGET(gtk_builder_get_object(builder, "shortcuts_window"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(self));
    gtk_window_present(GTK_WINDOW(win));
    g_object_unref(builder);
}

static void
on_about_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    const char *developers[] = {"Jeremy", NULL};
    
    AdwAboutDialog *dialog = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    adw_about_dialog_set_application_name(dialog, "BrightEyes");
    adw_about_dialog_set_application_icon(dialog, "org.brightEyes.BrightEyes");
    
    /* Explicitly load logo resource to ensure it appears */
    GdkTexture *logo = gdk_texture_new_from_resource("/org/brighteyes/BrightEyes/icons/hicolor/1024x1024/apps/org.brightEyes.BrightEyes.png");
    if (logo) {
        /* AdwAboutDialog usually derives its logo from the icon name.
           However, if for some reason resource lookup fails, we can try to force it 
           via GtkAboutDialog properties if they are respected, or just rely on the name.
           If 'application-logo' is not a property, this call is harmless data attachment.
           But to be safe, we mainly rely on the icon theme.
        */
        /* Try both common property names just in case of hidden support or mix-ins */
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(dialog), "logo-paintable"))
             g_object_set(dialog, "logo-paintable", logo, NULL);
             
        g_object_unref(logo);
    }
    
    adw_about_dialog_set_developers(dialog, developers);
    adw_about_dialog_set_version(dialog, "0.1");
    adw_about_dialog_set_copyright(dialog, "© 2026 Jeremy");
    adw_about_dialog_set_website(dialog, "https://github.com/jeremy/BrightEyes");
    adw_about_dialog_set_issue_url(dialog, "https://github.com/jeremy/BrightEyes/issues");
    adw_about_dialog_set_license_type(dialog, GTK_LICENSE_GPL_3_0);
    
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static gboolean
on_drop(GtkDropTarget *target,
        const GValue  *value,
        double         x,
        double         y,
        gpointer       user_data)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(user_data);
    
    if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        GFile *file = g_value_get_object(value);
        char *path = g_file_get_path(file);
        
        if (path) {
            /* Check if directory or file */
            if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
                 curator_load_directory(self->curator, path);
                 const char *current = curator_get_current(self->curator);
                 load_image_path(self, current);
                 thumbnails_bar_refresh(self->thumbnails);
                 adw_overlay_split_view_set_show_sidebar(self->split_view, TRUE);
            } else {
                 bright_eyes_window_open_file(self, path);
            }
            g_free(path);
            return TRUE;
        }
    }
    return FALSE;
}

static void
bright_eyes_window_dispose(GObject *object)
{
    BrightEyesWindow *self = BRIGHT_EYES_WINDOW(object);
    
    if (self->slideshow_id > 0) {
        g_source_remove(self->slideshow_id);
        self->slideshow_id = 0;
    }

    /* Disconnect signals from child widgets before they are destroyed */
    if (self->viewer) {
        g_signal_handlers_disconnect_by_data(self->viewer, self);
    }
    if (self->thumbnails) {
        g_signal_handlers_disconnect_by_data(self->thumbnails, self);
    }

    g_clear_object(&self->selected_editor);
    if (self->editor_candidates) {
        g_list_free_full(self->editor_candidates, g_object_unref);
        self->editor_candidates = NULL;
    }

    g_clear_pointer(&self->ocr_language, g_free);
    g_clear_object(&self->curator);

    /* Chain up to destroy widgets */
    G_OBJECT_CLASS(bright_eyes_window_parent_class)->dispose(object);
}

static void
bright_eyes_window_class_init(BrightEyesWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = bright_eyes_window_dispose;
}

static void
bright_eyes_window_init(BrightEyesWindow *self)
{
    self->curator = curator_new();
    self->slideshow_id = 0;    self->slideshow_duration = 3;
    self->ocr_language = g_strdup("eng");
    self->viewer_dark_background = TRUE;
    self->confirm_delete = TRUE;
    self->default_fit_to_window = TRUE;

    /* Load settings */
    load_settings(self);

    gtk_window_set_default_size(GTK_WINDOW(self), 1000, 700);
    gtk_window_set_title(GTK_WINDOW(self), "BrightEyes");

    /* Inner Split View (Metadata) - Sidebar at End */
    self->metadata_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_position(self->metadata_view, GTK_PACK_END);
    adw_overlay_split_view_set_show_sidebar(self->metadata_view, FALSE);

    /* Construct Metadata Sidebar */
    self->metadata_sidebar = metadata_sidebar_new();
    gtk_widget_set_size_request(self->metadata_sidebar, 250, -1);
    
    adw_overlay_split_view_set_sidebar(self->metadata_view, self->metadata_sidebar);

    /* Main toast overlay to hold content */
    self->toast_overlay = adw_toast_overlay_new();

    /* Content for Metadata View is the Viewer wrapped in an Overlay */
    self->viewer = viewer_new();
    viewer_set_dark_background(self->viewer, self->viewer_dark_background);
    viewer_set_default_fit(self->viewer, self->default_fit_to_window);
    
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(self->viewer));

    /* Key Controller for Shortcuts */
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_controller);

    /* HUD for Next/Prev arrows */
    GtkWidget *hud_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_halign(hud_box, GTK_ALIGN_END);
    gtk_widget_set_valign(hud_box, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(hud_box, 20);
    gtk_widget_set_margin_end(hud_box, 24);
    
    GtkWidget *prev_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(prev_btn, "osd");
    gtk_widget_set_size_request(prev_btn, 40, 40);
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev_clicked), self);
    
    GtkWidget *next_btn = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(next_btn, "osd");
    gtk_widget_set_size_request(next_btn, 40, 40);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next_clicked), self);

    gtk_box_append(GTK_BOX(hud_box), prev_btn);
    gtk_box_append(GTK_BOX(hud_box), next_btn);
    
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), hud_box);

    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), overlay);
    adw_overlay_split_view_set_content(self->metadata_view, self->toast_overlay);
    
    /* Connect viewer signals */
    g_signal_connect(self->viewer, "zoom-changed", G_CALLBACK(on_viewer_zoom_changed), self);
    g_signal_connect_swapped(self->viewer, "open-requested", G_CALLBACK(show_open_folder_dialog), self);

    /* Outer Split View (Thumbnails) - Sidebar at Start */
    self->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_position(self->split_view, GTK_PACK_START);
    
    self->thumbnails = thumbnails_bar_new(self->curator);
    g_signal_connect(self->thumbnails, "file-activated", G_CALLBACK(on_thumbnail_activated), self);
    adw_overlay_split_view_set_sidebar(self->split_view, GTK_WIDGET(self->thumbnails));
    adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE); /* Hidden until folder selected */
    
    /* Content for Outer View is the Inner View */
    adw_overlay_split_view_set_content(self->split_view, GTK_WIDGET(self->metadata_view));

    /* Header Bar */
    GtkWidget *header = adw_header_bar_new();
    
    /* Pack Start: Open Files, Then Viewer Controls */
    GtkWidget *open_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(open_btn, "Open File");
    g_signal_connect_swapped(open_btn, "clicked", G_CALLBACK(show_open_dialog), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), open_btn);
    
    GtkWidget *folder_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(folder_btn, "Open Folder");
    g_signal_connect_swapped(folder_btn, "clicked", G_CALLBACK(show_open_folder_dialog), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), folder_btn);

    /* Separator */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sep);

    /* Viewer Controls moved to Start */
    GtkWidget *sidebar_btn = gtk_button_new_from_icon_name("view-grid-symbolic");
    gtk_widget_set_tooltip_text(sidebar_btn, "Toggle Thumbnails");
    g_signal_connect_swapped(sidebar_btn, "clicked", G_CALLBACK(toggle_sidebar), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebar_btn);
    
    /* Swapped Zoom Buttons: Minus then Plus */
    GtkWidget *zoom_out = gtk_button_new_from_icon_name("zoom-out-symbolic");
    gtk_widget_set_tooltip_text(zoom_out, "Zoom Out");
    g_signal_connect(zoom_out, "clicked", G_CALLBACK(on_zoom_out_clicked), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), zoom_out);

    GtkWidget *zoom_in = gtk_button_new_from_icon_name("zoom-in-symbolic");
    gtk_widget_set_tooltip_text(zoom_in, "Zoom In");
    g_signal_connect(zoom_in, "clicked", G_CALLBACK(on_zoom_in_clicked), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), zoom_in);
    
    GtkWidget *fit_btn = gtk_button_new_from_icon_name("zoom-fit-best-symbolic");
    gtk_widget_set_tooltip_text(fit_btn, "Fit to Window");
    g_signal_connect(fit_btn, "clicked", G_CALLBACK(on_fit_clicked), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), fit_btn);

    GtkWidget *rot_l = gtk_button_new_from_icon_name("object-rotate-left-symbolic");
    gtk_widget_set_tooltip_text(rot_l, "Rotate Left");
    g_signal_connect(rot_l, "clicked", G_CALLBACK(on_rotate_left_clicked), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), rot_l);
    
    GtkWidget *rot_r = gtk_button_new_from_icon_name("object-rotate-right-symbolic");
    gtk_widget_set_tooltip_text(rot_r, "Rotate Right");
    g_signal_connect(rot_r, "clicked", G_CALLBACK(on_rotate_right_clicked), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), rot_r);

    self->slideshow_btn = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(self->slideshow_btn, "Toggle Slideshow");
    g_signal_connect_swapped(self->slideshow_btn, "clicked", G_CALLBACK(toggle_slideshow), self);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), self->slideshow_btn);
    
    /* Pack End: Menu (Rightmost), then Metadata, then OCR */
    
    /* Setup actions for the menu */
    GSimpleActionGroup *actions = g_simple_action_group_new();
    
    GActionEntry action_entries[] = {
        { "preferences", on_preferences_action, NULL, NULL, NULL },
        { "shortcuts", on_shortcuts_action, NULL, NULL, NULL },
        { "about", on_about_action, NULL, NULL, NULL },
        { "open-editor", on_open_editor_action, NULL, NULL, NULL },
        { "ocr-whole", on_ocr_whole_action, NULL, NULL, NULL },
        { "ocr-selection", on_ocr_selection_action, NULL, NULL, NULL },
        { "clear-selection", on_clear_selection_action, NULL, NULL, NULL }
    };
    
    g_action_map_add_action_entries(G_ACTION_MAP(actions), action_entries, G_N_ELEMENTS(action_entries), self);
    gtk_widget_insert_action_group(GTK_WIDGET(self), "win", G_ACTION_GROUP(actions));
    g_object_unref(actions);
    
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "win.preferences");
    g_menu_append(menu, "Keyboard Shortcuts", "win.shortcuts");
    g_menu_append(menu, "About BrightEyes", "win.about");

    /* Cheeseburger / Menu Button */
    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu_btn, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_btn);
    g_object_unref(menu);
    
    /* Metadata Button (next to menu) */
    GtkWidget *metadata_btn = gtk_button_new_from_icon_name("emoji-objects-symbolic");
    gtk_widget_set_tooltip_text(metadata_btn, "Metadata");
    g_signal_connect_swapped(metadata_btn, "clicked", G_CALLBACK(toggle_metadata), self);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), metadata_btn);

    /* OCR Menu Button (next to metadata) */
    GMenu *ocr_menu = g_menu_new();
    g_menu_append(ocr_menu, "OCR Whole Image", "win.ocr-whole");
    g_menu_append(ocr_menu, "OCR Selection", "win.ocr-selection");
    g_menu_append(ocr_menu, "Clear Selection", "win.clear-selection");

    GtkWidget *ocr_btn = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(ocr_btn), G_MENU_MODEL(ocr_menu));
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(ocr_btn), "scanner-symbolic");
    gtk_widget_set_tooltip_text(ocr_btn, "OCR");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), ocr_btn);
    g_object_unref(ocr_menu);
    
    /* Status Bar */
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(status_bar, 4);
    gtk_widget_set_margin_bottom(status_bar, 4);
    gtk_widget_set_margin_start(status_bar, 12);
    gtk_widget_set_margin_end(status_bar, 12);
    
    self->status_label = gtk_label_new("Zoom: 100%");
    gtk_box_append(GTK_BOX(status_bar), self->status_label);

    /* Toolbar View */
    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), status_bar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(self->split_view));
    
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), toolbar_view);

    /* Key Controller - Removed duplicate */

    /* Drag and Drop */
    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drop_target));
}

BrightEyesWindow *
bright_eyes_window_new(GtkApplication *app)
{
    return g_object_new(TYPE_BRIGHT_EYES_WINDOW, "application", app, NULL);
}

void
bright_eyes_window_open_file(BrightEyesWindow *self, const char *path)
{
    if (path) {
        curator_set_current_file(self->curator, path);
        
        /* Get resolved path */
        const char *resolved = curator_get_current(self->curator);
        load_image_path(self, resolved);
        
        /* Refresh sidebar if implemented */
        thumbnails_bar_refresh(self->thumbnails);
    }
}
