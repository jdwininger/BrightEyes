#include "ocr.h"
#include <glib.h>
#include <gio/gio.h>
#ifdef __has_include
#  if __has_include(<tesseract/capi.h>)
#    include <tesseract/capi.h>
#  else
#    include <capi.h>
#  endif
#else
#  include <tesseract/capi.h>
#endif
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
    int min_confidence;
} OcrTaskData;

typedef struct {
    char *text;
    int confidence;
} OcrCandidate;

static PIX *preprocess_pix_for_ocr(PIX *pix);

static void
ocr_candidate_clear(OcrCandidate *candidate)
{
    if (!candidate) return;
    g_free(candidate->text);
    candidate->text = NULL;
    candidate->confidence = -1;
}

static void
configure_tesseract_for_accuracy(TessBaseAPI *api)
{
    if (!api) return;

    /* A few conservative defaults that usually help without changing the
     * app's UI or adding external dependencies. */
    TessBaseAPISetVariable(api, "user_defined_dpi", "300");
    TessBaseAPISetVariable(api, "preserve_interword_spaces", "1");
}

static OcrCandidate
run_ocr_pass(TessBaseAPI *api, PIX *pix, int psm)
{
    OcrCandidate candidate = { 0 };
    candidate.confidence = -1;

    if (!api || !pix) return candidate;

    TessBaseAPISetPageSegMode(api, (TessPageSegMode)psm);
    TessBaseAPIClear(api);
    TessBaseAPISetImage2(api, (struct Pix *)pix);

    char *out = TessBaseAPIGetUTF8Text(api);
    if (out) {
        candidate.text = g_strdup(out);
        candidate.confidence = TessBaseAPIMeanTextConf(api);
        TessDeleteText(out);
    } else {
        candidate.text = g_strdup("");
        candidate.confidence = -1;
    }

    return candidate;
}

static gboolean
candidate_is_better(const OcrCandidate *a, const OcrCandidate *b)
{
    if (!a || !a->text) return FALSE;
    if (!b || !b->text) return TRUE;

    if (a->confidence != b->confidence)
        return a->confidence > b->confidence;

    return strlen(a->text) > strlen(b->text);
}

static void
collect_best_candidate_from_pix(TessBaseAPI *api, PIX *pix, OcrCandidate *best)
{
    if (!api || !pix || !best) return;

    const int psm_candidates[] = { 6, 11, 7, 13 };
    for (guint i = 0; i < G_N_ELEMENTS(psm_candidates); i++) {
        OcrCandidate candidate = run_ocr_pass(api, pix, psm_candidates[i]);
        if (candidate_is_better(&candidate, best)) {
            ocr_candidate_clear(best);
            best->text = g_strdup(candidate.text ? candidate.text : "");
            best->confidence = candidate.confidence;
        }
        ocr_candidate_clear(&candidate);
    }
}

static PIX *
preprocess_pix_for_ocr(PIX *pix)
{
    if (!pix) return NULL;

    /* Work on a clone so the original image data is never modified. */
    PIX *work = pixClone(pix);
    if (!work) return NULL;

    /* Tesseract tends to do better on grayscale text than on color UI art. */
    if (pixGetDepth(work) > 8) {
        PIX *gray = pixConvertRGBToGrayFast(work);
        if (gray) {
            pixDestroy(&work);
            work = gray;
        }
    }

    /* Improve local contrast to help Tesseract separate characters from the
     * background on screenshots, scans, and low-contrast UI text. */
    {
        PIX *contrasted = pixContrastNorm(NULL, work, 5, 5, 2, 1, 1);
        if (contrasted) {
            pixDestroy(&work);
            work = contrasted;
        }
    }

    /* Deskew small image rotations so Tesseract sees straighter baselines.
     * This is kept conservative and only affects the in-memory OCR copy. */
    {
        PIX *deskewed = pixDeskew(work, 1);
        if (deskewed) {
            pixDestroy(&work);
            work = deskewed;
        }
    }

    /* Small text benefits from a modest upscale. Keep this conservative to
     * avoid blowing up large screenshots. */
    int w = pixGetWidth(work);
    int h = pixGetHeight(work);
    if (MAX(w, h) > 0 && MAX(w, h) < 1600) {
        PIX *scaled = pixScale(work, 2.0f, 2.0f);
        if (scaled) {
            pixDestroy(&work);
            work = scaled;
        }
    }

    return work;
}

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

    configure_tesseract_for_accuracy(api);

    PIX *pix = pixRead(data->path);
    if (!pix) {
        TessBaseAPIDelete(api);
        g_task_return_error(task, g_error_new(g_quark_from_static_string("ocr"), 3, "Failed to read image"));
        return;
    }

    PIX *work = preprocess_pix_for_ocr(pix);
    if (!work) {
        pixDestroy(&pix);
        TessBaseAPIDelete(api);
        g_task_return_error(task, g_error_new(g_quark_from_static_string("ocr"), 4, "Failed to preprocess image"));
        return;
    }

    OcrCandidate best = { 0 };
    best.confidence = -1;

    /* Try a few page segmentation modes and keep the strongest result.
     * This is a cheap way to improve OCR quality without shipping more
     * dependencies or exposing extra user settings. */
    collect_best_candidate_from_pix(api, work, &best);

    result_text = best.text ? g_strdup(best.text) : g_strdup("");
    ocr_candidate_clear(&best);

    pixDestroy(&work);
    pixDestroy(&pix);
    TessBaseAPIDelete(api);

    g_task_return_pointer(task, result_text, g_free);
}

void
ocr_recognize_image_async(const char *path, const char *lang, const char *datapath, int min_confidence, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    OcrTaskData *data = g_new0(OcrTaskData, 1);
    data->path = g_strdup(path);
    data->lang = lang ? g_strdup(lang) : g_strdup("eng");
    data->datapath = datapath ? g_strdup(datapath) : NULL;
    data->min_confidence = min_confidence;

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
