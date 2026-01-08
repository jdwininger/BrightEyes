#ifndef OCR_H
#define OCR_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Asynchronously recognizes text from an image file. "lang" may be NULL (defaults to "eng").
 * If datapath is non-NULL it is passed to TessBaseAPIInit3 so custom tessdata locations can be used.
 * On success, the result is a newly-allocated string (UTF-8) returned from
 * ocr_recognize_image_finish, which the caller must free with g_free(). */

void ocr_recognize_image_async(const char *path, const char *lang, const char *datapath, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
char *ocr_recognize_image_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* OCR_H */