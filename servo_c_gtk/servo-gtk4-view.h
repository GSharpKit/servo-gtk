/*
 * GTK4 web view widget backed by Servo.
 *
 * The widget is a plain GtkWidget holding one child that does the drawing:
 *
 *   - a GtkGLArea when GTK can give it a GL context (GL 3.2 / GLES 3.0 or
 *     newer). Servo then renders on the GPU in the area's own context and the
 *     frame is blitted into the area -- it never goes through the CPU.
 *   - a GtkDrawingArea otherwise. Servo rasterizes on the CPU with the swgl
 *     software rasterizer and the RGBA frames are painted with Cairo. No GL
 *     implementation, driver or GPU is needed by the widget or the engine.
 *
 * The choice is made per widget and falls back by itself: if the GL area
 * cannot be realized, or its context is unusable, the widget switches to the
 * drawing area. Set SERVO_GTK_RENDERER=cpu to skip the GL attempt.
 *
 * A GL web view is shut down when it is unrealized (its context goes away
 * with the GL area), and freeing the last web view shuts Servo down for the
 * process. Do not unrealize and re-realize the only web view -- e.g. by
 * moving it to another window -- and expect it to come back.
 *
 * GTK4 itself is a different matter. GSK's default renderer wants OpenGL,
 * which on Windows it reaches through libepoxy loading ANGLE's libEGL.dll and
 * libGLESv2.dll. On a machine with no GL those are missing and GTK4 may fail
 * before any window appears. If you target GL-less machines, select the Cairo
 * renderer before gtk_init():
 *
 *     g_setenv("GSK_RENDERER", "cairo", FALSE);
 *
 * (demo/servo-gtk4-demo.c does exactly this.) The web view then takes the CPU
 * path; with the Cairo renderer a GL area would be read back to the CPU anyway.
 */
#ifndef SERVO_GTK4_VIEW_H
#define SERVO_GTK4_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Opaque Servo webview handle, defined by the Rust FFI (libservoshell/servo-webview.h). */
typedef struct ServoWebViewHandle ServoWebViewHandle;

#define SERVO_GTK_TYPE_WEB_VIEW                (servo_gtk_web_view_get_type ())
#define SERVO_GTK_WEB_VIEW(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj), SERVO_GTK_TYPE_WEB_VIEW, ServoGtkWebView))
#define SERVO_GTK_TYPE_WEB_VIEW_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST ((klass), SERVO_GTK_TYPE_WEB_VIEW, ServoGtkWebViewClass))
#define SERVO_GTK_IS_WEB_VIEW(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SERVO_GTK_TYPE_WEB_VIEW))
#define SERVO_GTK_IS_WEB_VIEW_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), SERVO_GTK_TYPE_WEB_VIEW))
#define SERVO_GTK_IS_WEB_VIEW_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), SERVO_GTK_TYPE_WEB_VIEW, ServoGtkWebViewClass))


/**
 * ServoGtkLoadEvent:
 * @SERVO_GTK_LOAD_STARTED: a new page started loading
 * @SERVO_GTK_LOAD_HEAD_PARSED: the page's `<head>` has been parsed; its
 *   `<body>` now exists in the DOM
 * @SERVO_GTK_LOAD_FINISHED: the page and all its subresources (images,
 *   stylesheets, scripts) have loaded, i.e. `document.readyState` is
 *   `"complete"`
 *
 * The stages of a page load, reported by #ServoGtkWebView::load-changed.
 */
typedef enum {
    SERVO_GTK_LOAD_STARTED,
    SERVO_GTK_LOAD_HEAD_PARSED,
    SERVO_GTK_LOAD_FINISHED
} ServoGtkLoadEvent;

#define SERVO_GTK_TYPE_LOAD_EVENT (servo_gtk_load_event_get_type ())
GType servo_gtk_load_event_get_type (void) G_GNUC_CONST;

typedef struct _ServoGtkWebView              ServoGtkWebView;
typedef struct _ServoGtkWebViewPrivate       ServoGtkWebViewPrivate;
typedef struct _ServoGtkWebViewClass         ServoGtkWebViewClass;

GType servo_gtk_web_view_get_type (void) G_GNUC_CONST;

struct _ServoGtkWebView
{
    GtkWidget web_view;

    gchar *uri;

    /* Servo FFI state. */
    ServoWebViewHandle *servo;
    GdkPixbuf          *frame;
    guint               tick_id;

    /*< private >*/
    ServoGtkWebViewPrivate *priv;
};

struct _ServoGtkWebViewClass {
    GtkWidgetClass parent_class;

    /* Signals */
    void (*uri_changed) (ServoGtkWebView *web_view,
                         const gchar     *uri);
    void (*load_changed) (ServoGtkWebView   *web_view,
                          ServoGtkLoadEvent  load_event);

    /* Padding for future expansion */
    void (*_gtk_reserved2) (void);
    void (*_gtk_reserved3) (void);
    void (*_gtk_reserved4) (void);
};

/**
 * servo_gtk_web_view_new:
 *
 * Creates a new Servo GTK web view widget.
 *
 * Returns: (transfer full): a newly-created #ServoGtkWebView
 */
ServoGtkWebView *servo_gtk_web_view_new(void);

/**
 * servo_gtk_web_view_load_uri:
 * @self: a #ServoGtkWebView
 * @uri: URI to load
 *
 * Loads the given URI.
 */
void servo_gtk_web_view_load_uri(ServoGtkWebView *self, const gchar *uri);

/**
 * servo_gtk_web_view_get_uri:
 * @self: a #ServoGtkWebView
 *
 * Gets the currently loaded URI.
 *
 * Returns: (nullable): the current URI
 */
const gchar *servo_gtk_web_view_get_uri(ServoGtkWebView *self);

/**
 * servo_gtk_web_view_is_loading:
 * @self: a #ServoGtkWebView
 *
 * Whether a page is loading: %TRUE from #SERVO_GTK_LOAD_STARTED until
 * #SERVO_GTK_LOAD_FINISHED. Also available as the #ServoGtkWebView:is-loading
 * property.
 *
 * Returns: %TRUE while a page is loading
 */
gboolean servo_gtk_web_view_is_loading(ServoGtkWebView *self);

/**
 * ServoGtkScriptResultCallback:
 * @web_view: the #ServoGtkWebView the script ran in
 * @result_json: (nullable): the script's return value serialized as a JSON
 *   string, or %NULL if evaluation failed
 * @error: (nullable): a human-readable error message, or %NULL on success
 * @user_data: the user data passed to servo_gtk_web_view_evaluate_script()
 *
 * Invoked exactly once when an asynchronous script evaluation finishes.
 * Exactly one of @result_json and @error is non-%NULL. Both strings are only
 * valid for the duration of the call.
 */
typedef void (*ServoGtkScriptResultCallback) (
    ServoGtkWebView *web_view,
    const gchar     *result_json,
    const gchar     *error,
    gpointer         user_data
);

/**
 * servo_gtk_web_view_evaluate_script:
 * @self: a #ServoGtkWebView
 * @script: the JavaScript source to evaluate
 * @callback: (scope async) (closure user_data): callback invoked once with
 *   the result
 * @user_data: user data passed to @callback
 *
 * Asynchronously evaluates @script in the web view's top-level browsing
 * context. When evaluation finishes @callback is invoked exactly once, later,
 * from the GTK main loop.
 */
void servo_gtk_web_view_evaluate_script(
    ServoGtkWebView                  *self,
    const gchar                      *script,
    ServoGtkScriptResultCallback      callback,
    gpointer                          user_data
);

/**
 * ServoGtkPrintResultCallback:
 * @web_view: the #ServoGtkWebView the print was requested from
 * @printed: %TRUE if the document was handed to a printer, %FALSE if the user
 *   cancelled or printing failed
 * @error: (nullable): a human-readable error message, or %NULL when @printed
 *   is %TRUE or the user simply cancelled
 * @user_data: the user data passed to servo_gtk_web_view_print_pdf()
 *
 * Invoked exactly once when a print request finishes. @error is only non-%NULL
 * when something actually went wrong; a cancelled dialog reports
 * @printed = %FALSE with @error = %NULL. The string is only valid for the
 * duration of the call.
 */
typedef void (*ServoGtkPrintResultCallback) (
    ServoGtkWebView *web_view,
    gboolean         printed,
    const gchar     *error,
    gpointer         user_data
);

/**
 * servo_gtk_web_view_can_print:
 *
 * Whether this build has a printing backend, and therefore whether
 * servo_gtk_web_view_print_pdf() can do anything. Use it to decide whether to
 * offer a print action at all.
 *
 * Returns: %TRUE if printing is supported on this platform
 */
gboolean servo_gtk_web_view_can_print(void);

/**
 * servo_gtk_web_view_print_pdf:
 * @self: a #ServoGtkWebView
 * @path: (type filename): the PDF file to print
 * @callback: (scope async) (nullable) (closure user_data): invoked once when
 *   the request finishes, or %NULL to ignore the outcome
 * @user_data: user data passed to @callback
 *
 * Asks the user for a printer and sends @path to it.
 *
 * The file is spooled as it is on disk — it is never rendered by the web view,
 * so the printed result does not depend on how the document happens to display.
 * @path need not be the document currently shown; any readable PDF works.
 *
 * How the file reaches the printer is platform-specific. On Unix it is handed
 * to the print queue directly (CUPS accepts PDF). On Windows, where the spooler
 * will not generally take a PDF, it is passed to the application registered for
 * `.pdf`. Where neither is available servo_gtk_web_view_can_print() returns
 * %FALSE and this reports an error.
 *
 * @callback is invoked exactly once, later, from the GTK main loop.
 */
void servo_gtk_web_view_print_pdf(
    ServoGtkWebView             *self,
    const gchar                 *path,
    ServoGtkPrintResultCallback  callback,
    gpointer                     user_data
);

G_END_DECLS

#endif /* SERVO_GTK4_VIEW_H */
