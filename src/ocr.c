#include "ocr.h"
#include <glib.h>
#include <gio/gio.h>
#include <tesseract/capi.h>
#include <leptonica/allheaders.h>

/* OCR (Tesseract) helpers
 *
 * Provides an asynchronous OCR API that runs Tesseract in a background
 * thread and returns the recognized text via GTask callbacks.
 *
 * Sections: task data, worker thread, public async API.
 */

/* Internal data passed to worker thread */
typedef struct {
    char *path;
    char *lang;
    char *datapath;
} OcrTaskData;

static void
ocr_task_data_free(OcrTaskData *data)
{
    if (!data) return;
    g_free(data->path);
    g_free(data->lang);
    g_free(data->datapath);
    g_free(data);
}

static void
ocr_task_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    OcrTaskData *data = task_data;
    char *result_text = NULL;

    TessBaseAPI *api = TessBaseAPICreate();
    if (!api) {
        g_task_return_error(task, g_error_new(g_quark_from_static_string("ocr"), 1, "Tesseract allocator failed"));
        return;
    }

    const char *lang = data->lang ? data->lang : "eng";
    const char *datapath = data->datapath && *data->datapath ? data->datapath : NULL;

    if (TessBaseAPIInit3(api, datapath, lang) != 0) {
        TessBaseAPIDelete(api);
        g_task_return_error(task, g_error_new(g_quark_from_static_string("ocr"), 2, "Tesseract init failed (language missing?)"));
        return;
    }

    PIX *pix = pixRead(data->path);
    if (!pix) {
        TessBaseAPIDelete(api);
        g_task_return_error(task, g_error_new(g_quark_from_static_string("ocr"), 3, "Failed to read image"));
        return;
    }

    TessBaseAPISetImage2(api, (struct Pix *)pix);
    char *out = TessBaseAPIGetUTF8Text(api);
    if (out) {
        result_text = g_strdup(out);
        TessDeleteText(out);
    } else {
        result_text = g_strdup("");
    }

    pixDestroy(&pix);
    TessBaseAPIDelete(api);

    g_task_return_pointer(task, result_text, g_free);
}

void
ocr_recognize_image_async(const char *path, const char *lang, const char *datapath, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    OcrTaskData *data = g_new0(OcrTaskData, 1);
    data->path = g_strdup(path);
    data->lang = lang ? g_strdup(lang) : g_strdup("eng");
    data->datapath = datapath ? g_strdup(datapath) : NULL;

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, data, (GDestroyNotify)ocr_task_data_free);
    g_task_run_in_thread(task, ocr_task_thread);
    g_object_unref(task);
}

char *
ocr_recognize_image_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}
