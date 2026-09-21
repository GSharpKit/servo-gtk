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


typedef struct _ServoGtkWebView              ServoGtkWebView;
typedef struct _ServoGtkWebViewPrivate       ServoGtkWebViewPrivate;
typedef struct _ServoGtkWebViewClass         ServoGtkWebViewClass;

GType servo_gtk_web_view_get_type (void) G_GNUC_CONST;

struct _ServoGtkWebView
{
    GtkDrawingArea web_view;

    gchar *uri;

    /* Servo FFI state. */
    ServoWebViewHandle *servo;
    GdkPixbuf          *frame;
    guint               tick_id;

    /*< private >*/
    ServoGtkWebViewPrivate *priv;
};

struct _ServoGtkWebViewClass {
    GtkDrawingAreaClass parent_class;

    /* Signals */
    void (*uri_changed) (ServoGtkWebView *web_view,
                         const gchar     *uri);

    /* Padding for future expansion */
    void (*_gtk_reserved1) (void);
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
