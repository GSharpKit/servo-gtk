/*
 * C ABI for libservoshell — a thin wrapper around Servo's WebView.
 *
 * All functions must be called from a single thread (the GTK main thread):
 * the underlying Servo objects are not thread-safe.
 *
 * Typical use from a GtkDrawingArea:
 *   1. servo_webview_new()
 *   2. servo_webview_set_frame_ready_callback() -> copy RGBA into a texture
 *   3. drive servo_webview_spin() from a GtkWidget tick callback
 *   4. forward input via servo_webview_pointer_* / servo_webview_scroll
 *   5. servo_webview_free() on dispose
 */
#ifndef SERVO_WEBVIEW_H
#define SERVO_WEBVIEW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a single Servo webview. */
typedef struct ServoWebViewHandle ServoWebViewHandle;

/*
 * Invoked once per rendered frame with a tightly-packed RGBA8 buffer
 * (stride == width * 4). The pointer is valid only for the duration of the
 * call — copy the pixels before returning.
 */
typedef void (*ServoFrameReadyCallback)(const uint8_t *rgba,
                                        uint32_t       width,
                                        uint32_t       height,
                                        void          *user_data);

/*
 * Invoked when the page requests a different cursor. `name` is a
 * NUL-terminated CSS cursor name (e.g. "pointer"), valid only for the call.
 */
typedef void (*ServoCursorChangedCallback)(const char *name,
                                           void       *user_data);

/*
 * Create a webview with an initial surface of width x height device pixels.
 * `initial_uri` is the URL to load on creation, or NULL to start on
 * about:blank. The initial URL MUST be supplied here rather than via a
 * separate servo_webview_load_uri() call immediately after creation: Servo
 * creates the browsing context together with this URL, and a load issued
 * before that context exists is silently dropped. Returns NULL on failure;
 * free with servo_webview_free().
 */
ServoWebViewHandle *servo_webview_new(uint32_t    width,
                                      uint32_t    height,
                                      const char *initial_uri);

/* Destroy a handle. NULL is a no-op. */
void servo_webview_free(ServoWebViewHandle *webview);

/* Register/clear callbacks (pass NULL to clear). */
void servo_webview_set_frame_ready_callback(ServoWebViewHandle     *webview,
                                            ServoFrameReadyCallback callback,
                                            void                   *user_data);
void servo_webview_set_cursor_changed_callback(ServoWebViewHandle        *webview,
                                               ServoCursorChangedCallback callback,
                                               void                      *user_data);

/* Navigation. */
void servo_webview_load_uri(ServoWebViewHandle *webview, const char *uri);
void servo_webview_reload(ServoWebViewHandle *webview);
void servo_webview_go_back(ServoWebViewHandle *webview);
void servo_webview_go_forward(ServoWebViewHandle *webview);

/* Surface size in device pixels. */
void servo_webview_resize(ServoWebViewHandle *webview,
                          uint32_t            width,
                          uint32_t            height);

/* Input. `button`: 1 = left, 2 = middle, 3 = right (GDK numbering). */
void servo_webview_pointer_move(ServoWebViewHandle *webview, double x, double y);
void servo_webview_pointer_button(ServoWebViewHandle *webview,
                                  uint32_t            button,
                                  bool                pressed,
                                  double              x,
                                  double              y);
void servo_webview_scroll(ServoWebViewHandle *webview, double dx, double dy);

/*
 * Pump Servo's event loop once. Call regularly from a GTK tick/timeout source;
 * the frame-ready callback fires synchronously from inside this call.
 */
void servo_webview_spin(ServoWebViewHandle *webview);

/*
 * Current URL as a newly-allocated UTF-8 string, or NULL. Free with
 * servo_string_free().
 */
char *servo_webview_get_uri(ServoWebViewHandle *webview);

/* Free a string returned by this library. NULL is a no-op. */
void servo_string_free(char *string);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_WEBVIEW_H */
