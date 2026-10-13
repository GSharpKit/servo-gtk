/*
 * C ABI for libservoshell — a thin wrapper around Servo's WebView.
 *
 * All functions must be called from a single thread (the GTK main thread):
 * the underlying Servo objects are not thread-safe.
 *
 * Typical use from a GtkDrawingArea (CPU rendering, needs no GL at all):
 *   1. servo_webview_new()
 *   2. servo_webview_set_frame_ready_callback() -> copy RGBA into a texture
 *   3. drive servo_webview_spin() from a GtkWidget tick callback
 *   4. forward input via servo_webview_pointer_* / servo_webview_scroll
 *   5. servo_webview_free() on dispose
 *
 * Or from a GtkGLArea (GPU rendering in the area's own context):
 *   1. servo_webview_new_gl() once the area is realized; on NULL fall back
 *      to the GtkDrawingArea path above
 *   2. servo_webview_set_gl_frame_ready_callback() -> gtk_gl_area_queue_render()
 *   3. servo_webview_gl_blit() from the area's "render" handler
 *   4. as above
 *   5. servo_webview_free() before the area is unrealized
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
 * Invoked when the webview navigates to a new URL (link, redirect, history
 * traversal, or an embedder-issued load). `url` is a NUL-terminated UTF-8
 * string valid only for the duration of the call — copy it before returning.
 */
typedef void (*ServoUrlChangedCallback)(const char *url,
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

/*
 * Makes the host's GL context current on the calling thread; returns whether
 * it succeeded (false: no context, issue no GL).
 */
typedef bool (*ServoGlMakeCurrentCallback)(void *user_data);

/*
 * Invoked once per painted frame on a servo_webview_new_gl() handle. The frame
 * is in the handle's offscreen framebuffer; schedule a redraw and copy it out
 * with servo_webview_gl_blit().
 */
typedef void (*ServoGlFrameReadyCallback)(void *user_data);

/*
 * Like servo_webview_new(), but Servo renders on the GPU inside a GL context
 * the host owns, e.g. a GtkGLArea's. That context must exist, `make_current`
 * must make it current (gtk_gl_area_make_current() and no gtk_gl_area_get_error()),
 * and it must outlive the handle: Servo calls `make_current` from this thread
 * during this call, while painting and during servo_webview_free(). The RGBA
 * frame callback is never invoked for such a handle.
 *
 * Returns NULL if the context is unusable (older than GL 3.2 / GLES 3.0, not
 * owned by EGL/GLX/WGL/CGL, ...) or Servo has been shut down; fall back to
 * servo_webview_new() in the first case.
 */
ServoWebViewHandle *servo_webview_new_gl(uint32_t                   width,
                                         uint32_t                   height,
                                         const char                *initial_uri,
                                         ServoGlMakeCurrentCallback make_current,
                                         void                      *user_data);

/* Destroy a handle. NULL is a no-op. */
void servo_webview_free(ServoWebViewHandle *webview);

/* Register/clear callbacks (pass NULL to clear). */
void servo_webview_set_frame_ready_callback(ServoWebViewHandle     *webview,
                                            ServoFrameReadyCallback callback,
                                            void                   *user_data);
void servo_webview_set_gl_frame_ready_callback(ServoWebViewHandle       *webview,
                                               ServoGlFrameReadyCallback callback,
                                               void                     *user_data);

/*
 * Copy the last painted frame of a servo_webview_new_gl() handle into the
 * framebuffer currently bound for drawing, scaled to width x height device
 * pixels. Call from the GtkGLArea "render" handler. Returns false (drawing
 * nothing) for a CPU handle.
 */
bool servo_webview_gl_blit(ServoWebViewHandle *webview,
                           uint32_t            width,
                           uint32_t            height);

void servo_webview_set_cursor_changed_callback(ServoWebViewHandle        *webview,
                                               ServoCursorChangedCallback callback,
                                               void                      *user_data);
void servo_webview_set_url_changed_callback(ServoWebViewHandle     *webview,
                                            ServoUrlChangedCallback callback,
                                            void                   *user_data);

/*
 * Load status values passed to ServoLoadStatusChangedCallback. Part of the
 * ABI; keep in sync with `servo_load_status` in servo-webview.rs.
 */
enum {
    SERVO_LOAD_STATUS_STARTED     = 0, /* navigation began, headers not parsed */
    SERVO_LOAD_STATUS_HEAD_PARSED = 1, /* <head> parsed, <body> exists */
    SERVO_LOAD_STATUS_COMPLETE    = 2  /* document and subresources loaded */
};

/*
 * Invoked when the page's load status changes. Same-document navigations
 * (fragments, history.pushState) report nothing.
 */
typedef void (*ServoLoadStatusChangedCallback)(uint32_t status, void *user_data);

void servo_webview_set_load_status_changed_callback(ServoWebViewHandle            *webview,
                                                    ServoLoadStatusChangedCallback callback,
                                                    void                          *user_data);

/*
 * Asked before the page navigates: link, location assignment or form
 * submission. Return true to allow, false to cancel. `url` is valid only for
 * the call. Navigations started through this API (load_uri, reload, history),
 * HTTP redirects and an <iframe> loading its src are not asked about. Runs inside
 * servo_webview_spin(); must not free the handle.
 */
typedef bool (*ServoNavigationRequestCallback)(const char *url, void *user_data);

void servo_webview_set_navigation_request_callback(ServoWebViewHandle            *webview,
                                                   ServoNavigationRequestCallback callback,
                                                   void                          *user_data);

/* The current SERVO_LOAD_STATUS_* value; COMPLETE for a NULL handle. */
uint32_t servo_webview_get_load_status(ServoWebViewHandle *webview);

/*
 * Console log levels passed to ServoConsoleMessageCallback. Part of the ABI;
 * keep in sync with the `servo_console` module in servo-webview.rs.
 */
typedef enum {
    SERVO_CONSOLE_LOG   = 0,
    SERVO_CONSOLE_DEBUG = 1,
    SERVO_CONSOLE_INFO  = 2,
    SERVO_CONSOLE_WARN  = 3,
    SERVO_CONSOLE_ERROR = 4,
    SERVO_CONSOLE_TRACE = 5,
    SERVO_CONSOLE_DIR   = 6
} ServoConsoleLevel;

/*
 * Invoked for every console.* message logged by the page. `message` is a
 * NUL-terminated UTF-8 string valid only for the duration of the call.
 * Indispensable when debugging a page that renders blank: without it, script
 * errors and warnings are discarded.
 */
typedef void (*ServoConsoleMessageCallback)(uint32_t    level,
                                            const char *message,
                                            void       *user_data);

void servo_webview_set_console_message_callback(ServoWebViewHandle         *webview,
                                                ServoConsoleMessageCallback callback,
                                                void                       *user_data);

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
/*
 * Scroll by (dx, dy) at surface position (x, y). Servo scrolls the scrollable
 * area under that point, so it must be the pointer's real position: a fixed
 * point scrolls whatever sits there instead of what the user is pointing at.
 */
void servo_webview_scroll(ServoWebViewHandle *webview,
                          double              dx,
                          double              dy,
                          double              x,
                          double              y);

/*
 * Named keys understood by servo_webview_key(). SERVO_KEY_CHARACTER means the
 * key produced text — pass its Unicode codepoint in the `unicode` argument. All
 * other values identify a non-printable key and ignore `unicode`. These values
 * are part of the ABI; keep them in sync with the `servo_key` module in
 * servo-webview.rs.
 */
typedef enum {
    SERVO_KEY_CHARACTER = 0,
    SERVO_KEY_UNIDENTIFIED = 1,
    SERVO_KEY_ENTER = 2,
    SERVO_KEY_TAB = 3,
    SERVO_KEY_BACKSPACE = 4,
    SERVO_KEY_DELETE = 5,
    SERVO_KEY_ESCAPE = 6,
    SERVO_KEY_ARROW_LEFT = 7,
    SERVO_KEY_ARROW_RIGHT = 8,
    SERVO_KEY_ARROW_UP = 9,
    SERVO_KEY_ARROW_DOWN = 10,
    SERVO_KEY_HOME = 11,
    SERVO_KEY_END = 12,
    SERVO_KEY_PAGE_UP = 13,
    SERVO_KEY_PAGE_DOWN = 14
} ServoKey;

/* Modifier bitmask for servo_webview_key(). */
typedef enum {
    SERVO_MODIFIER_NONE    = 0,
    SERVO_MODIFIER_SHIFT   = 1 << 0,
    SERVO_MODIFIER_CONTROL = 1 << 1,
    SERVO_MODIFIER_ALT     = 1 << 2,
    SERVO_MODIFIER_META    = 1 << 3
} ServoModifier;

/*
 * Report a key press (`pressed`) or release. `key` is a ServoKey; when it is
 * SERVO_KEY_CHARACTER, `unicode` carries the typed character's Unicode
 * codepoint (0 otherwise). `modifiers` is a bitmask of ServoModifier flags.
 */
void servo_webview_key(ServoWebViewHandle *webview,
                       uint32_t            key,
                       uint32_t            unicode,
                       uint32_t            modifiers,
                       bool                pressed);

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


typedef void (*ServoScriptResultCallback)(const char *result_json,
                                          const char *error,
                                          void       *user_data);

void servo_webview_evaluate_script(ServoWebViewHandle        *webview,
                                   const char                *script,
                                   ServoScriptResultCallback  callback,
                                   void                      *user_data);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_WEBVIEW_H */
