/*
 * GTK3 web view widget backed by Servo.
 *
 * The widget is a GtkBin holding one child that does the drawing:
 *
 *   - a GtkGLArea when GTK can give it a GL context (GL 3.2 / GLES 3.0 or
 *     newer). Servo then renders on the GPU in the area's own context and the
 *     frame is blitted into the area.
 *   - a GtkDrawingArea otherwise. Servo rasterizes on the CPU with the swgl
 *     software rasterizer and the RGBA frames are painted with Cairo. No GL
 *     implementation, driver or GPU is needed by the widget or the engine.
 *
 * The choice is made per widget and falls back by itself: if the GL area
 * cannot be realized, or its context is unusable, the widget switches to the
 * drawing area. Set SERVO_GTK_RENDERER=cpu to skip the GL attempt.
 *
 * Do not add or remove the child yourself.
 *
 * A GL web view is shut down when it is unrealized (its context goes away
 * with the GL area). Servo itself runs from servo_gtk_init() to
 * servo_gtk_release(), so web views can be closed and new ones opened at any
 * time in between.
 */
#ifndef SERVO_GTK_WEB_VIEW_H
#define SERVO_GTK_WEB_VIEW_H

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

/**
 * ServoGtkContextMenuFlags:
 * @SERVO_GTK_CONTEXT_MENU_NONE: nothing in particular was under the pointer
 * @SERVO_GTK_CONTEXT_MENU_LINK: a link; its URI is passed along
 * @SERVO_GTK_CONTEXT_MENU_IMAGE: an image; its URI is passed along
 * @SERVO_GTK_CONTEXT_MENU_EDITABLE_TEXT: editable text, e.g. an input field
 * @SERVO_GTK_CONTEXT_MENU_SELECTION: the click was on selected text
 *
 * What a #ServoGtkWebView::context-menu was opened on. A link around an image
 * sets both @SERVO_GTK_CONTEXT_MENU_LINK and @SERVO_GTK_CONTEXT_MENU_IMAGE.
 */
typedef enum { /*< flags >*/
    SERVO_GTK_CONTEXT_MENU_NONE          = 0,
    SERVO_GTK_CONTEXT_MENU_LINK          = 1 << 0,
    SERVO_GTK_CONTEXT_MENU_IMAGE         = 1 << 1,
    SERVO_GTK_CONTEXT_MENU_EDITABLE_TEXT = 1 << 2,
    SERVO_GTK_CONTEXT_MENU_SELECTION     = 1 << 3
} ServoGtkContextMenuFlags;

#define SERVO_GTK_TYPE_CONTEXT_MENU_FLAGS (servo_gtk_context_menu_flags_get_type ())
GType servo_gtk_context_menu_flags_get_type (void) G_GNUC_CONST;

/*
 * Detailed action names of the entries the engine puts in a context menu,
 * for servo_gtk_context_menu_remove() or to put one back in another place.
 * Each is enabled only when it applies (Copy only with a selection, ...).
 */
#define SERVO_GTK_CONTEXT_MENU_ACTION_GO_BACK         "context-menu.go-back"
#define SERVO_GTK_CONTEXT_MENU_ACTION_GO_FORWARD      "context-menu.go-forward"
#define SERVO_GTK_CONTEXT_MENU_ACTION_RELOAD          "context-menu.reload"
#define SERVO_GTK_CONTEXT_MENU_ACTION_COPY_LINK       "context-menu.copy-link"
#define SERVO_GTK_CONTEXT_MENU_ACTION_COPY_IMAGE_LINK "context-menu.copy-image-link"
#define SERVO_GTK_CONTEXT_MENU_ACTION_CUT             "context-menu.cut"
#define SERVO_GTK_CONTEXT_MENU_ACTION_COPY            "context-menu.copy"
#define SERVO_GTK_CONTEXT_MENU_ACTION_PASTE           "context-menu.paste"
#define SERVO_GTK_CONTEXT_MENU_ACTION_SELECT_ALL      "context-menu.select-all"

typedef struct _ServoGtkWebView              ServoGtkWebView;
typedef struct _ServoGtkWebViewPrivate       ServoGtkWebViewPrivate;
typedef struct _ServoGtkWebViewClass         ServoGtkWebViewClass;

GType servo_gtk_web_view_get_type (void) G_GNUC_CONST;

struct _ServoGtkWebView
{
    GtkBin web_view;

    gchar *uri;

    /* Servo FFI state. */
    ServoWebViewHandle *servo;
    GdkPixbuf          *frame;
    guint               create_idle_id;
    guint               tick_id;

    /*< private >*/
    ServoGtkWebViewPrivate *priv;
};

struct _ServoGtkWebViewClass {
    GtkBinClass parent_class;

    /* Signals */
    void (*uri_changed) (ServoGtkWebView *web_view,
                         const gchar     *uri);
    void (*load_changed) (ServoGtkWebView   *web_view,
                          ServoGtkLoadEvent  load_event);
    gboolean (*decide_navigation) (ServoGtkWebView *web_view,
                                   const gchar     *uri);
    gboolean (*context_menu) (ServoGtkWebView          *web_view,
                              GMenu                    *menu,
                              ServoGtkContextMenuFlags  flags,
                              const gchar              *link_uri,
                              const gchar              *image_uri);

    /* Padding for future expansion */
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
 * servo_gtk_context_menu_remove:
 * @menu: the menu passed to #ServoGtkWebView::context-menu
 * @action: a detailed action name, e.g. %SERVO_GTK_CONTEXT_MENU_ACTION_COPY
 *
 * Removes every entry that activates @action from @menu, looking into its
 * sections too; a section left empty is removed with it.
 *
 * Returns: %TRUE if anything was removed
 */
gboolean servo_gtk_context_menu_remove(GMenu *menu, const gchar *action);

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
 * servo_gtk_init:
 * @data_dir: (type filename) (nullable): directory to keep cookies in, or
 *   %NULL to keep them in memory only
 *
 * Makes Servo available to #ServoGtkWebView. Call it before the first web
 * view is created -- until then web views stay blank -- and call
 * servo_gtk_release() when done, typically around g_application_run().
 *
 * Servo itself starts with the first web view, and keeps running while there
 * is none until servo_gtk_release(), so web views can be closed and new ones
 * opened at any time. It cannot be started again in the same process once it
 * has shut down.
 *
 * Cookies are read from `cookie_jar.json` in @data_dir when Servo starts and
 * written back when it shuts down; the directory is created if it does not
 * exist. Servo keeps `hsts_list.json`, `localstorage.json` and
 * `auth_cache.json` there too. The last holds HTTP authentication credentials
 * in plain text, so use a directory only the user can read, e.g. one below
 * g_get_user_data_dir().
 *
 * Calls are counted and each needs its own servo_gtk_release(). Only the
 * first sets the data directory; @data_dir is ignored by later ones.
 *
 * Returns: %TRUE on success; %FALSE if Servo has already been shut down or
 *   @data_dir cannot be used, in which case servo_gtk_release() must not be
 *   called for it
 */
gboolean servo_gtk_init(const gchar *data_dir);

/**
 * servo_gtk_release:
 *
 * Undoes one servo_gtk_init(). The last one shuts Servo down, which writes the
 * cookies to the data directory -- or, if web views still exist, the last of
 * them to be freed does. Destroy every web view first: a Servo still running
 * when the process exits crashes it.
 *
 * No web view can be created after Servo has shut down.
 */
void servo_gtk_release(void);

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

#endif /* SERVO_GTK_WEB_VIEW_H */
