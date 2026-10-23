#include "servo-gtk4-view.h"

#include "servo-webview.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>

/*
 * Printing has two backends, because the platforms differ fundamentally in
 * what their spooler will accept:
 *
 *   Unix  - CUPS takes a PDF as-is, so gtk-unix-print can hand the file
 *           straight to the queue (GtkPrintJob + gtk_print_job_set_source_file).
 *   Win32 - the print spooler is driver-based and will not generally accept a
 *           PDF, so the file is handed to whichever application is registered
 *           for .pdf via the shell's "printto"/"print" verb.
 *
 * Either way the document is spooled as it is on disk. It is never rendered
 * here and never goes through Servo, so printed output does not depend on how
 * the engine draws the document.
 */
#if defined(HAVE_GTK_UNIX_PRINT)
#define SERVO_GTK_CAN_PRINT 1
#include <gtk/gtkunixprint.h>
#elif defined(G_OS_WIN32)
#define SERVO_GTK_CAN_PRINT 1
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdk/win32/gdkwin32.h>
#endif

enum {
    PROP_0,
    PROP_URI,
    PROP_IS_LOADING,
    N_PROPERTIES,
    /* GtkScrollable, overridden rather than installed, so these deliberately
     * sit past N_PROPERTIES and have no entry in `properties`. */
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY
};

static GParamSpec *properties[N_PROPERTIES] = { NULL };

enum {
    URI_CHANGED,
    LOAD_CHANGED,
    DECIDE_NAVIGATION,
    CONTEXT_MENU,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/*
 * Scroll state, kept in the (previously unused) private pointer so the public
 * struct keeps its layout.
 *
 * Servo has no API for the scroll offset or the scrollable extent: it
 * only accepts relative scroll events. The numbers a scrollbar needs therefore
 * come from the page itself, by evaluating a small script on a timer, and a
 * drag is applied by assigning scrollLeft/scrollTop the same way. That also
 * targets the right element on pages that scroll an inner container rather
 * than the document - which is exactly what the PDF.js viewer does.
 */
struct _ServoGtkWebViewPrivate {
    GtkAdjustment *hadjustment;
    GtkAdjustment *vadjustment;
    guint          hscroll_policy : 1;
    guint          vscroll_policy : 1;
    guint          poll_id;
    /* A metrics round-trip is in flight; do not queue another. */
    gboolean       metrics_pending;
    /* Adjustments are being updated from the page, so value-changed must not
     * be echoed back to it. */
    gboolean       syncing;
    /* Between SERVO_GTK_LOAD_STARTED and SERVO_GTK_LOAD_FINISHED. */
    gboolean       is_loading;
    /* Last pointer position, because GtkEventControllerScroll does not carry
     * one and Servo scrolls whatever sits under the given point. */
    gdouble        pointer_x;
    gdouble        pointer_y;
    /* The one child, which does the drawing: a GtkGLArea while use_gl is set
     * (Servo renders into its context), a GtkDrawingArea otherwise. */
    GtkWidget     *area;
    gboolean       use_gl;
    /* The GL area's framebuffer size in device pixels, from its "resize". */
    gint           gl_width;
    gint           gl_height;
    /* A switch from the GL area to the drawing area is queued. */
    guint          fallback_id;
    /* The context menu Servo asked for, until it is answered. */
    ServoContextMenu   *context_menu;
    GtkWidget          *context_popover;
    GSimpleActionGroup *context_actions;
    /* The popover closed; answer and destroy it from an idle. */
    guint               context_closed_id;
};

G_DEFINE_TYPE_WITH_CODE(ServoGtkWebView, servo_gtk_web_view, GTK_TYPE_WIDGET,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))

/*
 * Report the scroll geometry of whatever the page actually scrolls, as
 * [left, top, scrollWidth, scrollHeight, clientWidth, clientHeight].
 *
 * Usually that is the document, but a viewer app often scrolls an inner
 * overflow container instead (PDF.js uses #viewerContainer), so fall back to
 * the largest scrollable element. The choice is cached on the window because
 * this runs several times a second, and revalidated whenever it stops being
 * scrollable or leaves the document.
 */
static const gchar *SERVO_GTK_SCROLL_METRICS_JS =
    "(function(){"
    "var d=document,w=window;"
    "function sc(c){return !!c&&(c.scrollHeight>c.clientHeight+1||c.scrollWidth>c.clientWidth+1)}"
    "function ar(c){return c?c.clientWidth*c.clientHeight:0}"
    /* Only a scroller filling a decent share of the viewport is the page's
       main one. Without this floor an incidental little overflow box - a
       toolbar dropdown mid-load - gets latched onto and never released. */
    "var min=0.25*w.innerWidth*w.innerHeight,e=w.__servoGtkScroller;"
    "if(!e||!e.isConnected||!sc(e)||ar(e)<min){"
    "e=d.scrollingElement||d.documentElement;"
    "if(!sc(e)){var best=null,ba=min,els=d.querySelectorAll('div,main,section,body');"
    "for(var i=0;i<els.length;i++){var c=els[i];"
    "if(sc(c)){var a=ar(c);if(a>ba){ba=a;best=c}}}"
    "if(best)e=best}"
    "w.__servoGtkScroller=e}"
    "return e?[e.scrollLeft,e.scrollTop,e.scrollWidth,e.scrollHeight,e.clientWidth,e.clientHeight]:[0,0,0,0,0,0]"
    "})()";

/* How often the page is asked for its scroll geometry, in milliseconds. */
#define SERVO_GTK_SCROLL_POLL_MS 200

/* Matches GdkPixbufDestroyNotify; frees the RGBA buffer owned by the pixbuf. */
static void
servo_gtk_web_view_free_frame_data(guchar *pixels, gpointer data)
{
    (void) data;
    g_free(pixels);
}

/*
 * Servo delivers a finished frame as a tightly-packed RGBA8 buffer that is only
 * valid for the duration of the callback, so we copy it into a GdkPixbuf (which
 * stores RGBA natively) and request a redraw. Runs on the main thread, inside
 * servo_webview_spin() from the tick callback.
 */
static void
servo_gtk_web_view_on_frame_ready(const guint8 *rgba,
                                  guint32       width,
                                  guint32       height,
                                  gpointer      user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    gsize    size = (gsize) width * height * 4;
    guint8  *copy = g_memdup2(rgba, size);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
        copy,
        GDK_COLORSPACE_RGB,
        TRUE,                 /* has_alpha */
        8,                    /* bits_per_sample */
        (int) width,
        (int) height,
        (int) (width * 4),    /* rowstride */
        servo_gtk_web_view_free_frame_data,
        NULL
    );

    g_clear_object(&self->frame);
    self->frame = pixbuf;

    /* The child caches its own render node, so it is the one to invalidate. */
    gtk_widget_queue_draw(self->priv->area);
}

/*
 * GL path: Servo has painted a frame into its offscreen framebuffer, inside
 * the GL area's context. Have the area render, which blits it.
 */
static void
servo_gtk_web_view_on_gl_frame_ready(gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    gtk_gl_area_queue_render(GTK_GL_AREA(self->priv->area));
}

/*
 * ServoGlMakeCurrentCallback: Servo needs the GL area's context. Going through
 * GDK rather than EGL/GLX/WGL directly keeps GDK's idea of the current context
 * right, which it relies on to skip redundant MakeCurrent calls.
 */
static bool
servo_gtk_web_view_gl_make_current(void *user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    GtkGLArea       *area = GTK_GL_AREA(self->priv->area);

    if (!gtk_widget_get_realized(GTK_WIDGET(area))) {
        return false;
    }

    gtk_gl_area_make_current(area);

    return gtk_gl_area_get_error(area) == NULL && gtk_gl_area_get_context(area) != NULL;
}

/*
 * Servo asked the embedder to change the pointer cursor. GTK4 resolves a named
 * cursor for the widget directly, with no GdkWindow round-trip.
 */
static void
servo_gtk_web_view_on_cursor_changed(const char *name, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), name);
}

/*
 * Servo navigated to a new URL (link, redirect, history traversal or an
 * embedder-issued load). Keep the "uri" property in sync and emit the
 * "uri-changed" signal so observers can react.
 */
static void
servo_gtk_web_view_on_url_changed(const char *url, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    if (g_strcmp0(self->uri, url) == 0) {
        return;
    }

    g_free(self->uri);
    self->uri = g_strdup(url);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_URI]);
    g_signal_emit(self, signals[URI_CHANGED], 0, self->uri);
}

/* Update "is-loading", notifying only on an actual change. */
static void
servo_gtk_web_view_set_loading(ServoGtkWebView *self, gboolean loading)
{
    if (self->priv->is_loading == loading) {
        return;
    }

    self->priv->is_loading = loading;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_LOADING]);
}

/*
 * Servo's load status changed. "is-loading" is updated first, so a
 * "load-changed" handler already sees the new value.
 */
static void
servo_gtk_web_view_on_load_status_changed(uint32_t status, gpointer user_data)
{
    ServoGtkWebView   *self = SERVO_GTK_WEB_VIEW(user_data);
    ServoGtkLoadEvent  event;

    switch (status) {
    case SERVO_LOAD_STATUS_STARTED:     event = SERVO_GTK_LOAD_STARTED;     break;
    case SERVO_LOAD_STATUS_HEAD_PARSED: event = SERVO_GTK_LOAD_HEAD_PARSED; break;
    case SERVO_LOAD_STATUS_COMPLETE:    event = SERVO_GTK_LOAD_FINISHED;    break;
    default:                            return;
    }

    g_object_freeze_notify(G_OBJECT(self));
    servo_gtk_web_view_set_loading(self, event != SERVO_GTK_LOAD_FINISHED);
    g_signal_emit(self, signals[LOAD_CHANGED], 0, event);
    g_object_thaw_notify(G_OBJECT(self));
}

/*
 * ServoNavigationRequestCallback: the page wants to navigate. A
 * "decide-navigation" handler returning TRUE has handled it, so Servo must
 * not load it.
 */
static bool
servo_gtk_web_view_on_navigation_request(const char *url, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    gboolean         handled = FALSE;

    g_signal_emit(self, signals[DECIDE_NAVIGATION], 0, url, &handled);

    return !handled;
}

/*
 * Context menu entries, indexed by SERVO_CONTEXT_MENU_ACTION_*, as GAction
 * names in the "context-menu" group. NULL: not offered, because opening a
 * link or image in a new view needs a web view this widget cannot create.
 */
static const gchar *const servo_gtk_context_actions[] = {
    [SERVO_CONTEXT_MENU_ACTION_GO_BACK]                   = "go-back",
    [SERVO_CONTEXT_MENU_ACTION_GO_FORWARD]                = "go-forward",
    [SERVO_CONTEXT_MENU_ACTION_RELOAD]                    = "reload",
    [SERVO_CONTEXT_MENU_ACTION_COPY_LINK]                 = "copy-link",
    [SERVO_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WEB_VIEW] = NULL,
    [SERVO_CONTEXT_MENU_ACTION_COPY_IMAGE_LINK]           = "copy-image-link",
    [SERVO_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_VIEW]    = NULL,
    [SERVO_CONTEXT_MENU_ACTION_CUT]                       = "cut",
    [SERVO_CONTEXT_MENU_ACTION_COPY]                      = "copy",
    [SERVO_CONTEXT_MENU_ACTION_PASTE]                     = "paste",
    [SERVO_CONTEXT_MENU_ACTION_SELECT_ALL]                = "select-all",
};

/* Whether `action` is one this widget shows. */
static gboolean
servo_gtk_web_view_offers_action(guint32 action)
{
    return action < G_N_ELEMENTS(servo_gtk_context_actions) &&
           servo_gtk_context_actions[action] != NULL;
}

/* Answer the pending context menu with nothing chosen, if there is one. */
static void
servo_gtk_web_view_dismiss_context_menu(ServoGtkWebView *self)
{
    if (self->priv->context_menu != NULL) {
        servo_context_menu_dismiss(self->priv->context_menu);
        self->priv->context_menu = NULL;
    }
}

/*
 * "context-menu.*" action: the user chose one of Servo's entries. GTK may
 * report the menu closed before or after this, which is why closing only
 * answers the menu later, from an idle.
 */
static void
servo_gtk_web_view_on_context_action(GSimpleAction *action,
                                     GVariant      *parameter,
                                     gpointer       user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    const gchar     *name = g_action_get_name(G_ACTION(action));

    (void) parameter;

    if (self->priv->context_menu == NULL) {
        return;
    }

    for (guint32 i = 0; i < G_N_ELEMENTS(servo_gtk_context_actions); i++) {
        if (g_strcmp0(servo_gtk_context_actions[i], name) == 0) {
            servo_context_menu_select(self->priv->context_menu, i);
            self->priv->context_menu = NULL;
            return;
        }
    }
}

/* Take the popover down for good. */
static void
servo_gtk_web_view_destroy_context_popover(ServoGtkWebView *self)
{
    if (self->priv->context_closed_id != 0) {
        g_source_remove(self->priv->context_closed_id);
        self->priv->context_closed_id = 0;
    }
    g_clear_pointer(&self->priv->context_popover, gtk_widget_unparent);
}

static gboolean
servo_gtk_web_view_context_closed_idle(gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    self->priv->context_closed_id = 0;
    servo_gtk_web_view_dismiss_context_menu(self);
    servo_gtk_web_view_destroy_context_popover(self);

    return G_SOURCE_REMOVE;
}

static void
servo_gtk_web_view_on_context_popover_closed(GtkPopover *popover, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) popover;

    if (self->priv->context_closed_id == 0) {
        self->priv->context_closed_id =
            g_idle_add(servo_gtk_web_view_context_closed_idle, self);
    }
}

/*
 * Turn Servo's menu into a GMenu of "context-menu.*" actions, enabling each
 * action as Servo says. Servo's separators become sections.
 */
static GMenu *
servo_gtk_web_view_build_context_model(ServoGtkWebView *self, ServoContextMenu *menu)
{
    GMenu  *model = g_menu_new();
    GMenu  *section = NULL;
    size_t  n_items = servo_context_menu_get_n_items(menu);

    for (size_t i = 0; i < n_items; i++) {
        guint32      action = servo_context_menu_item_action(menu, i);
        const gchar *label = servo_context_menu_item_label(menu, i);

        if (action == SERVO_CONTEXT_MENU_SEPARATOR) {
            if (section != NULL) {
                g_menu_append_section(model, NULL, G_MENU_MODEL(section));
                g_clear_object(&section);
            }
            continue;
        }
        if (!servo_gtk_web_view_offers_action(action) || label == NULL) {
            continue;
        }

        g_simple_action_set_enabled(
            G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(self->priv->context_actions),
                                                       servo_gtk_context_actions[action])),
            servo_context_menu_item_enabled(menu, i));

        gchar *detailed = g_strconcat("context-menu.", servo_gtk_context_actions[action], NULL);
        if (section == NULL) {
            section = g_menu_new();
        }
        g_menu_append(section, label, detailed);
        g_free(detailed);
    }
    if (section != NULL) {
        g_menu_append_section(model, NULL, G_MENU_MODEL(section));
        g_object_unref(section);
    }

    return model;
}

/*
 * Let the application edit the menu: emit "context-menu". Returns FALSE when
 * nothing is to be shown -- a handler took over, or the menu ended up empty --
 * in which case Servo's menu has been answered already.
 */
static gboolean
servo_gtk_web_view_offer_context_menu(ServoGtkWebView *self,
                                      ServoContextMenu *menu,
                                      GMenu            *model)
{
    gboolean handled = FALSE;

    /* The FFI flag bits are the public ones. */
    g_signal_emit(self, signals[CONTEXT_MENU], 0, model,
                  (ServoGtkContextMenuFlags) servo_context_menu_get_flags(menu),
                  servo_context_menu_get_link_url(menu),
                  servo_context_menu_get_image_url(menu),
                  &handled);

    /* A handler may have run something that answered it (e.g. a reload
     * withdrew the menu); only an unanswered menu is still ours. */
    if (handled || g_menu_model_get_n_items(G_MENU_MODEL(model)) == 0) {
        servo_gtk_web_view_dismiss_context_menu(self);
        return FALSE;
    }

    return self->priv->context_menu != NULL;
}

/*
 * ServoContextMenuCallback: show Servo's menu as a popover at the pointer,
 * after the application had its say. The area Servo reports is the whole
 * clicked element, so the pointer is the better anchor.
 */
static void
servo_gtk_web_view_on_context_menu(ServoContextMenu *menu,
                                   int32_t           x,
                                   int32_t           y,
                                   int32_t           width,
                                   int32_t           height,
                                   gpointer          user_data)
{
    ServoGtkWebView        *self = SERVO_GTK_WEB_VIEW(user_data);
    ServoGtkWebViewPrivate *priv = self->priv;

    (void) x;
    (void) y;
    (void) width;
    (void) height;

    /* Only one menu at a time: a new right click replaces the old menu. */
    servo_gtk_web_view_dismiss_context_menu(self);
    servo_gtk_web_view_destroy_context_popover(self);
    priv->context_menu = menu;

    GMenu *model = servo_gtk_web_view_build_context_model(self, menu);

    if (!servo_gtk_web_view_offer_context_menu(self, menu, model)) {
        g_object_unref(model);
        return;
    }

    priv->context_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
    g_object_unref(model);
    gtk_widget_set_parent(priv->context_popover, GTK_WIDGET(self));
    gtk_popover_set_has_arrow(GTK_POPOVER(priv->context_popover), FALSE);
    gtk_widget_set_halign(priv->context_popover, GTK_ALIGN_START);
    gtk_popover_set_pointing_to(GTK_POPOVER(priv->context_popover),
                                &(GdkRectangle) { (int) priv->pointer_x,
                                                  (int) priv->pointer_y, 1, 1 });
    g_signal_connect(priv->context_popover, "closed",
                     G_CALLBACK(servo_gtk_web_view_on_context_popover_closed), self);
    gtk_popover_popup(GTK_POPOVER(priv->context_popover));
}

/* ServoContextMenuHiddenCallback: Servo withdrew the menu, e.g. on navigation. */
static void
servo_gtk_web_view_on_context_menu_hidden(gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    if (self->priv->context_popover != NULL) {
        gtk_popover_popdown(GTK_POPOVER(self->priv->context_popover));
    }
}

/* Pump Servo's event loop once per frame clock tick. */
static gboolean
servo_gtk_web_view_tick(GtkWidget     *widget,
                        GdkFrameClock *frame_clock,
                        gpointer       user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    (void) frame_clock;
    (void) user_data;

    if (self->servo != NULL) {
        servo_webview_spin(self->servo);
    }

    return G_SOURCE_CONTINUE;
}

/* Forward declaration: the poll timer is started/stopped as adjustments come
 * and go, and as Servo is created and torn down. */
static void servo_gtk_web_view_update_scroll_polling(ServoGtkWebView *self);

/* Pull up to `count` numbers out of a JSON array such as "[0,10,1200,...]". */
static guint
servo_gtk_web_view_parse_numbers(const gchar *json, gdouble *out, guint count)
{
    guint found = 0;

    if (json == NULL) {
        return 0;
    }

    for (const gchar *p = json; *p != '\0' && found < count; p++) {
        if (g_ascii_isdigit(*p) || *p == '-' || *p == '+' ||
            (*p == '.' && g_ascii_isdigit(p[1]))) {
            gchar *end = NULL;
            gdouble value = g_ascii_strtod(p, &end);

            if (end == p) {
                continue;
            }
            out[found++] = value;
            p = end - 1;
        }
    }

    return found;
}

/* Result of the metrics script: push the page's geometry into the adjustments. */
static void
servo_gtk_web_view_on_scroll_metrics(const char *result_json,
                                     const char *error,
                                     void       *user_data)
{
    ServoGtkWebView *self = user_data;
    gdouble          m[6];

    self->priv->metrics_pending = FALSE;

    if (error != NULL || servo_gtk_web_view_parse_numbers(result_json, m, 6) != 6) {
        g_object_unref(self);
        return;
    }

    gdouble left = m[0], top = m[1];
    gdouble full_width = m[2], full_height = m[3];
    gdouble page_width = m[4], page_height = m[5];

    /* Updating an adjustment emits value-changed; that must not be mistaken
     * for the user dragging the scrollbar. */
    self->priv->syncing = TRUE;

    if (self->priv->hadjustment != NULL) {
        gtk_adjustment_configure(self->priv->hadjustment, left, 0.0,
                                 MAX(full_width, page_width),
                                 page_width / 10.0, page_width * 0.9, page_width);
    }
    if (self->priv->vadjustment != NULL) {
        gtk_adjustment_configure(self->priv->vadjustment, top, 0.0,
                                 MAX(full_height, page_height),
                                 page_height / 10.0, page_height * 0.9, page_height);
    }

    self->priv->syncing = FALSE;
    g_object_unref(self);
}

static gboolean
servo_gtk_web_view_poll_scroll(gpointer data)
{
    ServoGtkWebView *self = data;

    if (self->servo == NULL || self->priv->metrics_pending) {
        return G_SOURCE_CONTINUE;
    }

    self->priv->metrics_pending = TRUE;
    servo_webview_evaluate_script(self->servo, SERVO_GTK_SCROLL_METRICS_JS,
                                  servo_gtk_web_view_on_scroll_metrics,
                                  g_object_ref(self));

    return G_SOURCE_CONTINUE;
}

/*
 * evaluate_script does nothing at all when the callback is NULL, so a
 * fire-and-forget script still needs somewhere to deliver its result.
 */
static void
servo_gtk_web_view_ignore_script_result(const char *result_json,
                                        const char *error,
                                        void       *user_data)
{
    (void) result_json;
    (void) error;
    (void) user_data;
}

/* The user moved a scrollbar: tell the page to scroll there. */
static void
servo_gtk_web_view_on_adjustment_changed(GtkAdjustment *adjustment, gpointer data)
{
    ServoGtkWebView *self = data;

    if (self->priv->syncing || self->servo == NULL) {
        return;
    }

    gboolean horizontal = (adjustment == self->priv->hadjustment);
    gchar   *script = g_strdup_printf(
        "(function(){var e=window.__servoGtkScroller||document.scrollingElement;"
        "if(e){e.%s=%.2f}return 0})()",
        horizontal ? "scrollLeft" : "scrollTop",
        gtk_adjustment_get_value(adjustment));

    servo_webview_evaluate_script(self->servo, script,
                                  servo_gtk_web_view_ignore_script_result, NULL);
    g_free(script);
}

/* Poll only while it can do something: Servo exists and someone is listening. */
static void
servo_gtk_web_view_update_scroll_polling(ServoGtkWebView *self)
{
    gboolean wanted = self->servo != NULL &&
        (self->priv->hadjustment != NULL || self->priv->vadjustment != NULL);

    if (wanted && self->priv->poll_id == 0) {
        self->priv->poll_id = g_timeout_add(SERVO_GTK_SCROLL_POLL_MS,
                                            servo_gtk_web_view_poll_scroll, self);
    } else if (!wanted && self->priv->poll_id != 0) {
        g_source_remove(self->priv->poll_id);
        self->priv->poll_id = 0;
    }
}

static void
servo_gtk_web_view_set_adjustment(ServoGtkWebView  *self,
                                  GtkAdjustment   **slot,
                                  GtkAdjustment    *adjustment)
{
    if (*slot == adjustment) {
        return;
    }

    if (*slot != NULL) {
        g_signal_handlers_disconnect_by_func(
            *slot, servo_gtk_web_view_on_adjustment_changed, self);
        g_clear_object(slot);
    }

    if (adjustment != NULL) {
        *slot = g_object_ref_sink(adjustment);
        g_signal_connect(*slot, "value-changed",
                         G_CALLBACK(servo_gtk_web_view_on_adjustment_changed), self);
    }

    servo_gtk_web_view_update_scroll_polling(self);
}

static void
servo_gtk_web_view_set_property(GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    switch (property_id) {
    case PROP_URI:
        servo_gtk_web_view_load_uri(self, g_value_get_string(value));
        break;

    case PROP_HADJUSTMENT:
        servo_gtk_web_view_set_adjustment(self, &self->priv->hadjustment,
                                          g_value_get_object(value));
        break;

    case PROP_VADJUSTMENT:
        servo_gtk_web_view_set_adjustment(self, &self->priv->vadjustment,
                                          g_value_get_object(value));
        break;

    case PROP_HSCROLL_POLICY:
        self->priv->hscroll_policy = g_value_get_enum(value);
        break;

    case PROP_VSCROLL_POLICY:
        self->priv->vscroll_policy = g_value_get_enum(value);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
servo_gtk_web_view_get_property(GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    switch (property_id) {
    case PROP_URI:
        g_value_set_string(value, self->uri);
        break;

    case PROP_IS_LOADING:
        g_value_set_boolean(value, self->priv->is_loading);
        break;

    case PROP_HADJUSTMENT:
        g_value_set_object(value, self->priv->hadjustment);
        break;

    case PROP_VADJUSTMENT:
        g_value_set_object(value, self->priv->vadjustment);
        break;

    case PROP_HSCROLL_POLICY:
        g_value_set_enum(value, self->priv->hscroll_policy);
        break;

    case PROP_VSCROLL_POLICY:
        g_value_set_enum(value, self->priv->vscroll_policy);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
servo_gtk_web_view_dispose(GObject *object)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    if (self->tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }

    if (self->priv != NULL) {
        if (self->priv->poll_id != 0) {
            g_source_remove(self->priv->poll_id);
            self->priv->poll_id = 0;
        }
        servo_gtk_web_view_set_adjustment(self, &self->priv->hadjustment, NULL);
        servo_gtk_web_view_set_adjustment(self, &self->priv->vadjustment, NULL);
    }

    if (self->priv != NULL && self->priv->fallback_id != 0) {
        g_source_remove(self->priv->fallback_id);
        self->priv->fallback_id = 0;
    }

    g_clear_object(&self->frame);

    /* A pending menu belongs to the web view about to be freed. */
    if (self->priv != NULL) {
        servo_gtk_web_view_dismiss_context_menu(self);
        servo_gtk_web_view_destroy_context_popover(self);
        g_clear_object(&self->priv->context_actions);
    }

    /* Before the child goes: a GL handle still needs the area's context. */
    if (self->servo != NULL) {
        servo_webview_free(self->servo);
        self->servo = NULL;
    }

    if (self->priv != NULL) {
        g_clear_pointer(&self->priv->area, gtk_widget_unparent);
    }

    G_OBJECT_CLASS(servo_gtk_web_view_parent_class)->dispose(object);
}

static void
servo_gtk_web_view_finalize(GObject *object)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(object);

    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->priv, g_free);

    G_OBJECT_CLASS(servo_gtk_web_view_parent_class)->finalize(object);
}

/*
 * GtkDrawingArea draw func (GTK4), CPU path: paint the latest Servo frame, or
 * a neutral background if none has arrived yet.
 */
static void
servo_gtk_web_view_draw(GtkDrawingArea *area,
                        cairo_t        *cr,
                        int             width,
                        int             height,
                        gpointer        user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) area;
    (void) width;
    (void) height;

    if (self->frame != NULL) {
        gdk_cairo_set_source_pixbuf(cr, self->frame, 0, 0);
        cairo_paint(cr);
    } else {
        /* No frame yet: paint a neutral background. */
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
    }
}

/*
 * GtkGLArea::render, GL path: GTK has made the area's context current and
 * bound its framebuffer; copy Servo's last frame into it. Before Servo exists
 * the offscreen framebuffer does not either, so paint the white background
 * rather than leave the area's new, black, buffer showing.
 */
static gboolean
servo_gtk_web_view_on_gl_render(GtkGLArea    *area,
                                GdkGLContext *context,
                                gpointer      user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) area;
    (void) context;

    if (self->servo == NULL) {
        return servo_webview_gl_clear();
    }

    return servo_webview_gl_blit(self->servo,
                                 (guint) MAX(1, self->priv->gl_width),
                                 (guint) MAX(1, self->priv->gl_height));
}

/*
 * GtkGLArea::resize reports the framebuffer size in device pixels, which is
 * what the blit has to fill. (Servo itself is sized from the allocation.)
 */
static void
servo_gtk_web_view_on_gl_resize(GtkGLArea *area,
                                gint       width,
                                gint       height,
                                gpointer   user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) area;

    self->priv->gl_width = width;
    self->priv->gl_height = height;
}

static void servo_gtk_web_view_ensure_servo(ServoGtkWebView *self, gint width, gint height);

/* GtkGLArea::realize (after): the context exists now, or failed to. */
static void
servo_gtk_web_view_on_gl_realize(GtkWidget *area, gpointer user_data)
{
    GtkWidget *widget = GTK_WIDGET(user_data);

    (void) area;

    servo_gtk_web_view_ensure_servo(SERVO_GTK_WEB_VIEW(widget),
                                    gtk_widget_get_width(widget),
                                    gtk_widget_get_height(widget));
}

/*
 * GtkGLArea::unrealize, which runs before the area destroys its context: the
 * GL handle's WebRender objects live in that context, so the handle has to go
 * now. Freeing the last handle shuts Servo down for the process.
 */
static void
servo_gtk_web_view_on_gl_unrealize(GtkWidget *area, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) area;

    if (self->servo != NULL) {
        servo_gtk_web_view_dismiss_context_menu(self);
        servo_gtk_web_view_destroy_context_popover(self);
        servo_webview_free(self->servo);
        self->servo = NULL;
        servo_gtk_web_view_update_scroll_polling(self);
        servo_gtk_web_view_set_loading(self, FALSE);
    }
}

/*
 * Whether to try the GL area. SERVO_GTK_RENDERER forces the choice when set:
 * "cpu" skips GL, "gl" demands it. With no override the default is host GL,
 * except on macOS: the GL stacks shipped with macOS GTK builds cannot drive
 * Servo's compositor (the frame comes back blank), so the CPU/swgl path is the
 * default there -- which is also this project's baseline (must work with no
 * GPU/GL). Set SERVO_GTK_RENDERER=gl to opt back in.
 */
static gboolean
servo_gtk_web_view_wants_gl(void)
{
    const gchar *renderer = g_getenv("SERVO_GTK_RENDERER");

    if (g_strcmp0(renderer, "cpu") == 0) {
        return FALSE;
    }
    if (g_strcmp0(renderer, "gl") == 0) {
        return TRUE;
    }

#ifdef __APPLE__
    return FALSE;
#else
    return TRUE;
#endif
}

/* Replace the drawing child with a GtkGLArea (use_gl) or a GtkDrawingArea. */
static void
servo_gtk_web_view_set_area(ServoGtkWebView *self, gboolean use_gl)
{
    ServoGtkWebViewPrivate *priv = self->priv;

    g_clear_pointer(&priv->area, gtk_widget_unparent);

    priv->use_gl = use_gl;
    priv->gl_width = 0;
    priv->gl_height = 0;

    if (use_gl) {
        priv->area = gtk_gl_area_new();
        g_signal_connect(priv->area, "render",
                         G_CALLBACK(servo_gtk_web_view_on_gl_render), self);
        g_signal_connect(priv->area, "resize",
                         G_CALLBACK(servo_gtk_web_view_on_gl_resize), self);
        g_signal_connect_after(priv->area, "realize",
                               G_CALLBACK(servo_gtk_web_view_on_gl_realize), self);
        g_signal_connect(priv->area, "unrealize",
                         G_CALLBACK(servo_gtk_web_view_on_gl_unrealize), self);
    } else {
        priv->area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(priv->area),
                                       servo_gtk_web_view_draw, self, NULL);
    }

    gtk_widget_set_parent(priv->area, GTK_WIDGET(self));
}

/*
 * The GL area has no usable context, or Servo could not use it. Swap in the
 * drawing area, from an idle because this is found out during realize or
 * allocation, where the widget tree must not change.
 */
static gboolean
servo_gtk_web_view_fallback_to_cpu(gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    GError          *error = gtk_gl_area_get_error(GTK_GL_AREA(self->priv->area));

    self->priv->fallback_id = 0;

    g_message("Servo: no usable GL context (%s); rendering on the CPU.",
              error != NULL ? error->message : "see the message above");

    servo_gtk_web_view_set_area(self, FALSE);
    servo_gtk_web_view_ensure_servo(self,
                                    gtk_widget_get_width(GTK_WIDGET(self)),
                                    gtk_widget_get_height(GTK_WIDGET(self)));

    return G_SOURCE_REMOVE;
}

/*
 * Create the Servo webview, width x height, once the widget has a size and, on
 * the GL path, the GL area has a context. Called from size_allocate and the
 * area's realize; whichever comes last does it.
 *
 * Any URI requested before this is passed as the initial URL: Servo creates
 * the browsing context together with it. A separate load here instead would
 * have to wait for that context and load about:blank first.
 */
static void
servo_gtk_web_view_ensure_servo(ServoGtkWebView *self, gint width, gint height)
{
    ServoGtkWebViewPrivate *priv = self->priv;

    if (self->servo != NULL || priv->fallback_id != 0 || width <= 0 || height <= 0) {
        return;
    }

    if (priv->use_gl) {
        GtkGLArea *area = GTK_GL_AREA(priv->area);

        if (!gtk_widget_get_realized(priv->area)) {
            return; /* servo_gtk_web_view_on_gl_realize calls back */
        }

        if (gtk_gl_area_get_error(area) == NULL) {
            self->servo = servo_webview_new_gl((guint) width, (guint) height, self->uri,
                                               servo_gtk_web_view_gl_make_current, self);
        }
        if (self->servo == NULL) {
            priv->fallback_id = g_idle_add(servo_gtk_web_view_fallback_to_cpu, self);
            return;
        }

        servo_webview_set_gl_frame_ready_callback(
            self->servo, servo_gtk_web_view_on_gl_frame_ready, self);
    } else {
        self->servo = servo_webview_new((guint) width, (guint) height, self->uri);
        if (self->servo == NULL) {
            /*
             * The CPU path needs no GL, driver or GPU, so this is not about
             * graphics hardware: most likely Servo was already shut down,
             * because the last web view was freed. The Rust side says why.
             */
            g_warning("Servo: the engine could not be created; the web view stays blank.");
            return;
        }

        servo_webview_set_frame_ready_callback(
            self->servo, servo_gtk_web_view_on_frame_ready, self);
    }

    servo_webview_set_cursor_changed_callback(
        self->servo, servo_gtk_web_view_on_cursor_changed, self);
    servo_webview_set_url_changed_callback(
        self->servo, servo_gtk_web_view_on_url_changed, self);
    servo_webview_set_load_status_changed_callback(
        self->servo, servo_gtk_web_view_on_load_status_changed, self);
    servo_webview_set_navigation_request_callback(
        self->servo, servo_gtk_web_view_on_navigation_request, self);
    servo_webview_set_context_menu_callback(
        self->servo, servo_gtk_web_view_on_context_menu,
        servo_gtk_web_view_on_context_menu_hidden, self);
    /* A new web view starts out loading its first page, so Servo never
     * reports that STARTED as a change; report it from here. */
    if (servo_webview_get_load_status(self->servo) != SERVO_LOAD_STATUS_COMPLETE) {
        servo_gtk_web_view_on_load_status_changed(
            servo_webview_get_load_status(self->servo), self);
    }
    /* Scroll geometry can only be read once there is a page to ask. */
    servo_gtk_web_view_update_scroll_polling(self);
}

/* The drawing child is the whole widget; it asks for no size of its own. */
static void
servo_gtk_web_view_measure(GtkWidget      *widget,
                           GtkOrientation  orientation,
                           int             for_size,
                           int            *minimum,
                           int            *natural,
                           int            *minimum_baseline,
                           int            *natural_baseline)
{
    (void) widget;
    (void) orientation;
    (void) for_size;
    (void) minimum_baseline;
    (void) natural_baseline;

    *minimum = 0;
    *natural = 0;
}

/*
 * Give the child the whole allocation, then create the Servo webview on the
 * first real size and resize it on later ones.
 */
static void
servo_gtk_web_view_size_allocate(GtkWidget *widget,
                                 int        width,
                                 int        height,
                                 int        baseline)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->priv->area != NULL) {
        gtk_widget_allocate(self->priv->area, width, height, baseline, NULL);
    }
    /* A popover child has to be positioned by its parent's allocation. */
    if (self->priv->context_popover != NULL) {
        gtk_popover_present(GTK_POPOVER(self->priv->context_popover));
    }

    if (self->servo == NULL) {
        servo_gtk_web_view_ensure_servo(self, width, height);
    } else {
        servo_webview_resize(self->servo, (guint) MAX(1, width), (guint) MAX(1, height));
    }
}

/*
 * The child is never focusable, so the default -- move focus into the
 * children -- would skip the web view on Tab. Take focus here instead.
 */
static gboolean
servo_gtk_web_view_focus(GtkWidget *widget, GtkDirectionType direction)
{
    (void) direction;

    if (gtk_widget_is_focus(widget)) {
        return FALSE;
    }

    return gtk_widget_grab_focus(widget);
}

/* GtkEventControllerMotion::motion: forward the pointer position to Servo. */
static void
servo_gtk_web_view_on_motion(GtkEventControllerMotion *controller,
                             gdouble                   x,
                             gdouble                   y,
                             gpointer                  user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;

    self->priv->pointer_x = x;
    self->priv->pointer_y = y;

    if (self->servo != NULL) {
        servo_webview_pointer_move(self->servo, x, y);
    }
}

/* GtkGestureClick::pressed: grab focus and forward the button press to Servo. */
static void
servo_gtk_web_view_on_pressed(GtkGestureClick *gesture,
                              gint             n_press,
                              gdouble          x,
                              gdouble          y,
                              gpointer         user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) n_press;

    gtk_widget_grab_focus(GTK_WIDGET(self));

    if (self->servo != NULL) {
        guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
        servo_webview_pointer_button(self->servo, button, TRUE, x, y);
    }
}

/* GtkGestureClick::released: forward the button release to Servo. */
static void
servo_gtk_web_view_on_released(GtkGestureClick *gesture,
                               gint             n_press,
                               gdouble          x,
                               gdouble          y,
                               gpointer         user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) n_press;

    if (self->servo != NULL) {
        guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
        servo_webview_pointer_button(self->servo, button, FALSE, x, y);
    }
}

/*
 * GtkEventControllerScroll::scroll: the controller already delivers scroll
 * deltas (dx, dy), including smooth-scroll steps, so no direction decoding is
 * needed as it was under GTK3's GdkEventScroll.
 *
 * Scaled the way GtkScrolledWindow does it, so the page scrolls like every
 * other scrollable in the application: touchpad (surface) deltas are already
 * pixels, and a wheel unit is page_size^(2/3) pixels.
 */
static gboolean
servo_gtk_web_view_on_scroll(GtkEventControllerScroll *controller,
                             gdouble                   dx,
                             gdouble                   dy,
                             gpointer                  user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    GtkWidget       *widget = GTK_WIDGET(self);

#if GTK_CHECK_VERSION(4, 8, 0)
    if (gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL)
#else
    (void) controller;
#endif
    {
        dx *= pow(MAX(1, gtk_widget_get_width(widget)), 2.0 / 3.0);
        dy *= pow(MAX(1, gtk_widget_get_height(widget)), 2.0 / 3.0);
    }

    if (self->servo != NULL) {
        servo_webview_scroll_pixels(self->servo, dx, dy,
                                    self->priv->pointer_x, self->priv->pointer_y);
    }

    return TRUE;
}

/* Translate a GDK modifier state mask to the Servo modifier bitmask. */
static uint32_t
servo_gtk_web_view_modifiers(GdkModifierType state)
{
    uint32_t mods = SERVO_MODIFIER_NONE;

    if (state & GDK_SHIFT_MASK) {
        mods |= SERVO_MODIFIER_SHIFT;
    }
    if (state & GDK_CONTROL_MASK) {
        mods |= SERVO_MODIFIER_CONTROL;
    }
    if (state & GDK_ALT_MASK) {
        mods |= SERVO_MODIFIER_ALT;
    }
    if (state & (GDK_META_MASK | GDK_SUPER_MASK)) {
        mods |= SERVO_MODIFIER_META;
    }

    return mods;
}

/*
 * Map a GDK keyval to a named ServoKey. Printable keys return
 * SERVO_KEY_CHARACTER; the caller then resolves the actual character via
 * gdk_keyval_to_unicode(). The non-printable keys handled here are intercepted
 * before that step so their control-character Unicode values never leak through.
 */
static ServoKey
servo_gtk_web_view_map_keyval(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:      return SERVO_KEY_ENTER;
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab:   return SERVO_KEY_TAB;
    case GDK_KEY_BackSpace:      return SERVO_KEY_BACKSPACE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:      return SERVO_KEY_DELETE;
    case GDK_KEY_Escape:         return SERVO_KEY_ESCAPE;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:        return SERVO_KEY_ARROW_LEFT;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:       return SERVO_KEY_ARROW_RIGHT;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:          return SERVO_KEY_ARROW_UP;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:        return SERVO_KEY_ARROW_DOWN;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:        return SERVO_KEY_HOME;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:         return SERVO_KEY_END;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:     return SERVO_KEY_PAGE_UP;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:   return SERVO_KEY_PAGE_DOWN;
    default:                     return SERVO_KEY_CHARACTER;
    }
}

/* Shared handler for key press (pressed = TRUE) and release (FALSE). */
static gboolean
servo_gtk_web_view_key(ServoGtkWebView *self,
                       guint            keyval,
                       GdkModifierType  state,
                       gboolean         pressed)
{
    if (self->servo == NULL) {
        return FALSE;
    }

    ServoKey key = servo_gtk_web_view_map_keyval(keyval);
    guint32  unicode = 0;

    if (key == SERVO_KEY_CHARACTER) {
        unicode = gdk_keyval_to_unicode(keyval);
        /*
         * Bare modifiers and function keys have no Unicode mapping; report them
         * as unidentified rather than as an empty character.
         */
        if (unicode == 0) {
            key = SERVO_KEY_UNIDENTIFIED;
        }
    }

    servo_webview_key(self->servo,
                      (uint32_t) key,
                      unicode,
                      servo_gtk_web_view_modifiers(state),
                      pressed);

    return TRUE;
}

/* GtkEventControllerKey::key-pressed (returns whether the key was handled). */
static gboolean
servo_gtk_web_view_on_key_pressed(GtkEventControllerKey *controller,
                                  guint                  keyval,
                                  guint                  keycode,
                                  GdkModifierType        state,
                                  gpointer               user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;
    (void) keycode;

    return servo_gtk_web_view_key(self, keyval, state, TRUE);
}

/* GtkEventControllerKey::key-released (void return). */
static void
servo_gtk_web_view_on_key_released(GtkEventControllerKey *controller,
                                   guint                  keyval,
                                   guint                  keycode,
                                   GdkModifierType        state,
                                   gpointer               user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;
    (void) keycode;

    servo_gtk_web_view_key(self, keyval, state, FALSE);
}

static void
servo_gtk_web_view_class_init(ServoGtkWebViewClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->set_property = servo_gtk_web_view_set_property;
    object_class->get_property = servo_gtk_web_view_get_property;
    object_class->dispose = servo_gtk_web_view_dispose;
    object_class->finalize = servo_gtk_web_view_finalize;

    widget_class->measure = servo_gtk_web_view_measure;
    widget_class->size_allocate = servo_gtk_web_view_size_allocate;
    widget_class->focus = servo_gtk_web_view_focus;

    properties[PROP_URI] =
        g_param_spec_string(
            "uri",
            "URI",
            "The currently loaded URI",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
        );

    /**
     * ServoGtkWebView:is-loading:
     *
     * Whether a page is loading, from #SERVO_GTK_LOAD_STARTED until
     * #SERVO_GTK_LOAD_FINISHED.
     */
    properties[PROP_IS_LOADING] =
        g_param_spec_boolean(
            "is-loading",
            "Is loading",
            "Whether a page is loading",
            FALSE,
            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS
        );

    g_object_class_install_properties(object_class, N_PROPERTIES, properties);

    /* GtkScrollable: implementing it is what lets the widget simply be dropped
     * into a GtkScrolledWindow and get real scrollbars. */
    g_object_class_override_property(object_class, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(object_class, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(object_class, PROP_VSCROLL_POLICY, "vscroll-policy");

    /**
     * ServoGtkWebView::uri-changed:
     * @self: the #ServoGtkWebView
     * @uri: the new URI
     *
     * Emitted whenever the webview navigates to a new URL (link activation,
     * redirect, history traversal or an embedder-issued load).
     */
    signals[URI_CHANGED] =
        g_signal_new(
            "uri-changed",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_FIRST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, uri_changed),
            NULL, NULL, /* accumulator */
            NULL,       /* default (generic) C marshaller */
            G_TYPE_NONE,
            1,
            G_TYPE_STRING
        );

    /**
     * ServoGtkWebView::load-changed:
     * @self: the #ServoGtkWebView
     * @load_event: the #ServoGtkLoadEvent
     *
     * Emitted as a page loads: %SERVO_GTK_LOAD_STARTED when navigation to a
     * new document begins, %SERVO_GTK_LOAD_HEAD_PARSED once its `<head>` is
     * parsed and %SERVO_GTK_LOAD_FINISHED once it and all its subresources
     * have loaded.
     *
     * Same-document navigations -- fragment links, `history.pushState()` --
     * are not loads and emit only #ServoGtkWebView::uri-changed. Nor is going
     * back or forward to a page Servo still holds in memory: that is shown
     * again without reloading, and also emits only uri-changed. A page that
     * fails to load still finishes: Servo shows its error page instead.
     */
    signals[LOAD_CHANGED] =
        g_signal_new(
            "load-changed",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_FIRST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, load_changed),
            NULL, NULL, /* accumulator */
            NULL,       /* default (generic) C marshaller */
            G_TYPE_NONE,
            1,
            SERVO_GTK_TYPE_LOAD_EVENT
        );

    /**
     * ServoGtkWebView::decide-navigation:
     * @self: the #ServoGtkWebView
     * @uri: where the page wants to navigate
     *
     * Emitted before the page navigates: a link is followed, `location` is
     * assigned or a form is submitted. Return %TRUE to
     * stop it -- e.g. because the application handles @uri itself -- or
     * %FALSE to let it load. With no handler every navigation proceeds.
     *
     * Not emitted for loads the application starts itself
     * (servo_gtk_web_view_load_uri()), for history traversal or reload, for
     * HTTP redirects, for an `<iframe>` loading its `src`, or for
     * same-document navigations. The decision is
     * synchronous. Do not destroy the web view from a handler; defer that to
     * an idle.
     *
     * Returns: %TRUE to stop the navigation, %FALSE to allow it
     */
    signals[DECIDE_NAVIGATION] =
        g_signal_new(
            "decide-navigation",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, decide_navigation),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            1,
            G_TYPE_STRING
        );
    /**
     * ServoGtkWebView::context-menu:
     * @self: the #ServoGtkWebView
     * @menu: the menu about to be shown, editable
     * @flags: what the menu was opened on
     * @link_uri: (nullable): the link under the pointer, if any
     * @image_uri: (nullable): the image under the pointer, if any
     *
     * Emitted before a context menu is shown, with the entries the engine
     * proposes: Back, Forward, Reload, Copy, Copy Link, ... each enabled only
     * when it applies. They activate the `SERVO_GTK_CONTEXT_MENU_ACTION_*`
     * actions, and run inside the engine (Copy reaches the clipboard).
     *
     * Edit @menu to change what is shown. Remove entries with
     * servo_gtk_context_menu_remove() (or plain #GMenu calls), and add your
     * own with any action the web view can reach -- `win.` or `app.` actions,
     * or a group inserted on the web view. Use the action target to carry
     * @link_uri along, e.g. `g_menu_item_set_action_and_target_value (item,
     * "win.open-in-new-tab", g_variant_new_string (link_uri))`.
     *
     * Return %TRUE to show nothing (e.g. because you show a menu of your own);
     * an empty @menu is not shown either.
     *
     * Returns: %TRUE to suppress the menu, %FALSE to show @menu
     */
    signals[CONTEXT_MENU] =
        g_signal_new(
            "context-menu",
            G_TYPE_FROM_CLASS(klass),
            G_SIGNAL_RUN_LAST,
            G_STRUCT_OFFSET(ServoGtkWebViewClass, context_menu),
            g_signal_accumulator_true_handled, NULL,
            NULL,       /* default (generic) C marshaller */
            G_TYPE_BOOLEAN,
            4,
            G_TYPE_MENU,
            SERVO_GTK_TYPE_CONTEXT_MENU_FLAGS,
            G_TYPE_STRING,
            G_TYPE_STRING
        );
}

static void
servo_gtk_web_view_init(ServoGtkWebView *self)
{
    self->priv = g_new0(ServoGtkWebViewPrivate, 1);

    GtkWidget *widget = GTK_WIDGET(self);

    gtk_widget_set_focusable(widget, TRUE);

    /* GL first if allowed; servo_gtk_web_view_fallback_to_cpu swaps it out. */
    servo_gtk_web_view_set_area(self, servo_gtk_web_view_wants_gl());

    /*
     * GTK4 delivers input through event controllers rather than event masks and
     * per-event widget vfuncs. Each controller is owned by the widget once
     * added, so there is nothing to explicitly unref. Events land on the child
     * and bubble up to these, in this widget's coordinates -- which are the
     * child's, as it fills the widget.
     */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(servo_gtk_web_view_on_motion), self);
    gtk_widget_add_controller(widget, motion);

    GtkGesture *click = gtk_gesture_click_new();
    /* Button 0 == report every button, matching the GTK3 button handlers. */
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(servo_gtk_web_view_on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(servo_gtk_web_view_on_released), self);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(servo_gtk_web_view_on_scroll), self);
    gtk_widget_add_controller(widget, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(servo_gtk_web_view_on_key_pressed), self);
    g_signal_connect(key, "key-released", G_CALLBACK(servo_gtk_web_view_on_key_released), self);
    gtk_widget_add_controller(widget, key);

    /* The context menu's entries, enabled per menu from what Servo reports. */
    self->priv->context_actions = g_simple_action_group_new();
    for (guint i = 0; i < G_N_ELEMENTS(servo_gtk_context_actions); i++) {
        if (servo_gtk_context_actions[i] == NULL) {
            continue;
        }
        GSimpleAction *action = g_simple_action_new(servo_gtk_context_actions[i], NULL);
        g_signal_connect(action, "activate",
                         G_CALLBACK(servo_gtk_web_view_on_context_action), self);
        g_action_map_add_action(G_ACTION_MAP(self->priv->context_actions), G_ACTION(action));
        g_object_unref(action);
    }
    gtk_widget_insert_action_group(widget, "context-menu",
                                   G_ACTION_GROUP(self->priv->context_actions));

    self->tick_id = gtk_widget_add_tick_callback(widget, servo_gtk_web_view_tick, NULL, NULL);
}

ServoGtkWebView *
servo_gtk_web_view_new(void)
{
    return g_object_new(SERVO_GTK_TYPE_WEB_VIEW, NULL);
}

void
servo_gtk_web_view_load_uri(ServoGtkWebView *self, const gchar *uri)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (g_strcmp0(self->uri, uri) == 0) {
        return;
    }

    g_free(self->uri);
    self->uri = g_strdup(uri);

    /* If Servo is already up, load now; otherwise the resize handler picks it up. */
    if (self->servo != NULL && self->uri != NULL) {
        servo_webview_load_uri(self->servo, self->uri);
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_URI]);
}

const gchar *
servo_gtk_web_view_get_uri(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), NULL);

    return self->uri;
}

gboolean
servo_gtk_web_view_is_loading(ServoGtkWebView *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_WEB_VIEW(self), FALSE);

    return self->priv->is_loading;
}

gboolean
servo_gtk_context_menu_remove(GMenu *menu, const gchar *action)
{
    GMenuModel *model = G_MENU_MODEL(menu);
    gboolean    removed = FALSE;

    g_return_val_if_fail(G_IS_MENU(menu), FALSE);
    g_return_val_if_fail(action != NULL, FALSE);

    /* Backwards, so removing does not shift what is still to be visited. */
    for (gint i = g_menu_model_get_n_items(model) - 1; i >= 0; i--) {
        GMenuModel *section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
        gchar      *item_action = NULL;

        if (section != NULL) {
            if (G_IS_MENU(section) &&
                servo_gtk_context_menu_remove(G_MENU(section), action)) {
                removed = TRUE;
                if (g_menu_model_get_n_items(section) == 0) {
                    g_menu_remove(menu, i);
                }
            }
            g_object_unref(section);
            continue;
        }

        if (g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION,
                                            "s", &item_action) &&
            g_strcmp0(item_action, action) == 0) {
            g_menu_remove(menu, i);
            removed = TRUE;
        }
        g_free(item_action);
    }

    return removed;
}

GType
servo_gtk_context_menu_flags_get_type(void)
{
    static gsize type = 0;

    if (g_once_init_enter(&type)) {
        static const GFlagsValue values[] = {
            { SERVO_GTK_CONTEXT_MENU_NONE,          "SERVO_GTK_CONTEXT_MENU_NONE",          "none" },
            { SERVO_GTK_CONTEXT_MENU_LINK,          "SERVO_GTK_CONTEXT_MENU_LINK",          "link" },
            { SERVO_GTK_CONTEXT_MENU_IMAGE,         "SERVO_GTK_CONTEXT_MENU_IMAGE",         "image" },
            { SERVO_GTK_CONTEXT_MENU_EDITABLE_TEXT, "SERVO_GTK_CONTEXT_MENU_EDITABLE_TEXT", "editable-text" },
            { SERVO_GTK_CONTEXT_MENU_SELECTION,     "SERVO_GTK_CONTEXT_MENU_SELECTION",     "selection" },
            { 0, NULL, NULL }
        };

        g_once_init_leave(&type, g_flags_register_static("ServoGtkContextMenuFlags", values));
    }

    return type;
}

GType
servo_gtk_load_event_get_type(void)
{
    static gsize type = 0;

    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            { SERVO_GTK_LOAD_STARTED,     "SERVO_GTK_LOAD_STARTED",     "started" },
            { SERVO_GTK_LOAD_HEAD_PARSED, "SERVO_GTK_LOAD_HEAD_PARSED", "head-parsed" },
            { SERVO_GTK_LOAD_FINISHED,    "SERVO_GTK_LOAD_FINISHED",    "finished" },
            { 0, NULL, NULL }
        };

        g_once_init_leave(&type, g_enum_register_static("ServoGtkLoadEvent", values));
    }

    return type;
}

/*
 * One-shot context bridging the Servo FFI callback (which only knows a
 * user_data pointer) back to the public GTK callback (which also receives the
 * originating web view). Heap-allocated because Servo delivers the result
 * asynchronously, from a later servo_webview_spin().
 */
typedef struct {
    ServoGtkWebView              *web_view;
    ServoGtkScriptResultCallback  callback;
    gpointer                      user_data;
} ScriptResultClosure;

/*
 * Trampoline matching ServoScriptResultCallback. Forwards the result (or
 * error) to the user callback, then releases the closure and the reference it
 * held on the web view. Invoked exactly once per evaluate call.
 */
static void
servo_gtk_web_view_on_script_result(const char *result_json,
                                    const char *error,
                                    void       *user_data)
{
    ScriptResultClosure *closure = user_data;

    closure->callback(closure->web_view, result_json, error, closure->user_data);

    g_object_unref(closure->web_view);
    g_free(closure);
}

void
servo_gtk_web_view_evaluate_script(ServoGtkWebView              *self,
                                   const gchar                  *script,
                                   ServoGtkScriptResultCallback  callback,
                                   gpointer                      user_data)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));

    if (callback == NULL) {
        return;
    }

    /* Servo is created lazily on the first size allocation; without it there is
     * no browsing context to run the script in. */
    if (self->servo == NULL) {
        callback(self, NULL, "web view is not realized yet", user_data);
        return;
    }

    /* Keep the web view alive until the (asynchronous) result arrives. */
    ScriptResultClosure *closure = g_new0(ScriptResultClosure, 1);
    closure->web_view  = g_object_ref(self);
    closure->callback  = callback;
    closure->user_data = user_data;

    servo_webview_evaluate_script(self->servo,
                                  script,
                                  servo_gtk_web_view_on_script_result,
                                  closure);
}


/* ------------------------------------------------------------------------
 * Printing
 * ------------------------------------------------------------------------ */

/*
 * Pending result of a print request. Heap-allocated so the outcome can be
 * delivered from the main loop after the dialog (and, on Unix, the spooler)
 * have finished, and so the web view stays alive until then.
 */
typedef struct {
    ServoGtkWebView             *web_view;
    ServoGtkPrintResultCallback  callback;
    gpointer                     user_data;
    gboolean                     printed;
    gchar                       *error;
} PrintResultClosure;

static gboolean
servo_gtk_web_view_deliver_print_result(gpointer data)
{
    PrintResultClosure *closure = data;

    closure->callback(closure->web_view, closure->printed, closure->error,
                      closure->user_data);

    g_object_unref(closure->web_view);
    g_free(closure->error);
    g_free(closure);

    return G_SOURCE_REMOVE;
}

/*
 * Report the outcome of a print request. Always deferred to the main loop, so
 * the callback contract is the same whether the request failed immediately,
 * was cancelled in the dialog, or completed in the spooler much later.
 */
static void
servo_gtk_web_view_print_done(ServoGtkWebView             *self,
                              ServoGtkPrintResultCallback  callback,
                              gpointer                     user_data,
                              gboolean                     printed,
                              const gchar                 *error)
{
    if (callback == NULL) {
        /* Nobody is listening, but a failure should not vanish silently. */
        if (error != NULL) {
            g_warning("servo_gtk_web_view_print_pdf: %s", error);
        }
        return;
    }

    PrintResultClosure *closure = g_new0(PrintResultClosure, 1);
    closure->web_view  = g_object_ref(self);
    closure->callback  = callback;
    closure->user_data = user_data;
    closure->printed   = printed;
    closure->error     = g_strdup(error);

    g_idle_add(servo_gtk_web_view_deliver_print_result, closure);
}

gboolean
servo_gtk_web_view_set_data_directory(const gchar *path)
{
    gchar *utf8 = NULL;
    gboolean ok;

    /* GLib filenames are UTF-8 on Windows already; elsewhere convert them. */
    if (path != NULL) {
        utf8 = g_filename_to_utf8(path, -1, NULL, NULL, NULL);
        if (utf8 == NULL)
            return FALSE;
    }

    ok = servo_webview_set_data_directory(utf8);
    g_free(utf8);
    return ok;
}

gboolean
servo_gtk_web_view_can_print(void)
{
#ifdef SERVO_GTK_CAN_PRINT
    return TRUE;
#else
    return FALSE;
#endif
}

/* The print dialog is owned by the window holding the web view, if any. */
static GtkWindow *
servo_gtk_web_view_parent_window(ServoGtkWebView *self)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));

    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

#if defined(HAVE_GTK_UNIX_PRINT)

/* Context for the spooler callback, which outlives the print dialog. */
typedef struct {
    ServoGtkWebView             *web_view;
    ServoGtkPrintResultCallback  callback;
    gpointer                     user_data;
} PrintJobClosure;

/* The print dialog is asynchronous in GTK4, so the request survives in here. */
typedef struct {
    ServoGtkWebView             *web_view;
    gchar                       *path;
    ServoGtkPrintResultCallback  callback;
    gpointer                     user_data;
} PrintRequestClosure;

static void
servo_gtk_web_view_on_print_job_complete(GtkPrintJob  *job,
                                         gpointer      user_data,
                                         const GError *error)
{
    PrintJobClosure *closure = user_data;

    (void) job;

    servo_gtk_web_view_print_done(closure->web_view, closure->callback,
                                  closure->user_data,
                                  error == NULL,
                                  error != NULL ? error->message : NULL);

    g_object_unref(closure->web_view);
    g_free(closure);
}

static void
servo_gtk_web_view_on_print_response(GtkDialog *dialog,
                                     int        response,
                                     gpointer   user_data)
{
    PrintRequestClosure *request = user_data;
    ServoGtkWebView     *self = request->web_view;

    if (response != GTK_RESPONSE_OK) {
        servo_gtk_web_view_print_done(self, request->callback, request->user_data,
                                      FALSE, NULL);
    } else {
        GtkPrintUnixDialog *print_dialog = GTK_PRINT_UNIX_DIALOG(dialog);
        GtkPrinter         *printer = gtk_print_unix_dialog_get_selected_printer(print_dialog);
        GtkPrintSettings   *settings = gtk_print_unix_dialog_get_settings(print_dialog);
        GtkPageSetup       *page_setup = gtk_print_unix_dialog_get_page_setup(print_dialog);

        /* Fail closed rather than spooling a PDF to a queue that cannot take one. */
        if (printer == NULL) {
            servo_gtk_web_view_print_done(self, request->callback, request->user_data,
                                          FALSE, "no printer selected");
        } else if (!gtk_printer_accepts_pdf(printer)) {
            gchar *message = g_strdup_printf(
                "printer \"%s\" does not accept PDF jobs, and the file is sent unchanged",
                gtk_printer_get_name(printer));

            servo_gtk_web_view_print_done(self, request->callback, request->user_data,
                                          FALSE, message);
            g_free(message);
        } else {
            gchar       *title = g_path_get_basename(request->path);
            GtkPrintJob *job = gtk_print_job_new(title, printer, settings, page_setup);
            GError      *error = NULL;

            if (!gtk_print_job_set_source_file(job, request->path, &error)) {
                gchar *message = g_strdup_printf("could not read \"%s\": %s",
                                                 request->path, error->message);

                servo_gtk_web_view_print_done(self, request->callback,
                                              request->user_data, FALSE, message);
                g_free(message);
                g_clear_error(&error);
            } else {
                PrintJobClosure *closure = g_new0(PrintJobClosure, 1);
                closure->web_view  = g_object_ref(self);
                closure->callback  = request->callback;
                closure->user_data = request->user_data;

                /* Takes its own references; the callback runs when spooling ends. */
                gtk_print_job_send(job, servo_gtk_web_view_on_print_job_complete,
                                   closure, NULL);
            }

            g_object_unref(job);
            g_free(title);
        }

        g_clear_object(&settings);
    }

    gtk_window_destroy(GTK_WINDOW(dialog));

    g_object_unref(request->web_view);
    g_free(request->path);
    g_free(request);
}

void
servo_gtk_web_view_print_pdf(ServoGtkWebView             *self,
                             const gchar                 *path,
                             ServoGtkPrintResultCallback  callback,
                             gpointer                     user_data)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));
    g_return_if_fail(path != NULL);

    GtkWidget *dialog = gtk_print_unix_dialog_new("Print", servo_gtk_web_view_parent_window(self));
    /* The file is spooled verbatim, so page handling is the printer's job. */
    gtk_print_unix_dialog_set_manual_capabilities(GTK_PRINT_UNIX_DIALOG(dialog),
                                                  GTK_PRINT_CAPABILITY_COPIES |
                                                  GTK_PRINT_CAPABILITY_COLLATE |
                                                  GTK_PRINT_CAPABILITY_REVERSE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    PrintRequestClosure *request = g_new0(PrintRequestClosure, 1);
    request->web_view  = g_object_ref(self);
    request->path      = g_strdup(path);
    request->callback  = callback;
    request->user_data = user_data;

    g_signal_connect(dialog, "response",
                     G_CALLBACK(servo_gtk_web_view_on_print_response), request);
    gtk_window_present(GTK_WINDOW(dialog));
}

#elif defined(G_OS_WIN32)

void
servo_gtk_web_view_print_pdf(ServoGtkWebView             *self,
                             const gchar                 *path,
                             ServoGtkPrintResultCallback  callback,
                             gpointer                     user_data)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));
    g_return_if_fail(path != NULL);

    GtkWindow  *parent = servo_gtk_web_view_parent_window(self);
    GdkSurface *surface = parent != NULL
        ? gtk_native_get_surface(GTK_NATIVE(parent))
        : NULL;
    PRINTDLGW   dialog;

    ZeroMemory(&dialog, sizeof dialog);
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = surface != NULL ? gdk_win32_surface_get_handle(surface) : NULL;
    /* Page ranges and copies belong to the registered reader, not to us. */
    dialog.Flags = PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;

    if (!PrintDlgW(&dialog)) {
        DWORD dialog_error = CommDlgExtendedError();

        /* Zero means the user simply cancelled. */
        if (dialog_error != 0) {
            gchar *message = g_strdup_printf("could not open the print dialog (0x%lx)",
                                             (unsigned long) dialog_error);

            servo_gtk_web_view_print_done(self, callback, user_data, FALSE, message);
            g_free(message);
        } else {
            servo_gtk_web_view_print_done(self, callback, user_data, FALSE, NULL);
        }
    } else {
        DEVNAMES *names = dialog.hDevNames != NULL
            ? (DEVNAMES *) GlobalLock(dialog.hDevNames)
            : NULL;

        if (names == NULL) {
            servo_gtk_web_view_print_done(self, callback, user_data, FALSE,
                                          "the print dialog returned no printer");
        } else {
            /* DEVNAMES offsets are in characters from the start of the struct. */
            const gunichar2 *device = (const gunichar2 *) names + names->wDeviceOffset;
            gboolean         is_default = (names->wDefault & DN_DEFAULTPRN) != 0;

            gchar     *printer = g_utf16_to_utf8(device, -1, NULL, NULL, NULL);
            gchar     *quoted = printer != NULL ? g_strdup_printf("\"%s\"", printer) : NULL;
            gunichar2 *wide_path = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
            gunichar2 *wide_args = quoted != NULL
                ? g_utf8_to_utf16(quoted, -1, NULL, NULL, NULL)
                : NULL;

            if (wide_path == NULL || wide_args == NULL) {
                servo_gtk_web_view_print_done(self, callback, user_data, FALSE,
                                              "could not encode the file name for Windows");
            } else {
                INT_PTR result = (INT_PTR) ShellExecuteW(dialog.hwndOwner, L"printto",
                                                         (LPCWSTR) wide_path,
                                                         (LPCWSTR) wide_args,
                                                         NULL, SW_HIDE);

                if (result <= 32 && is_default) {
                    /* "print" always uses the default printer, which is what
                     * the user chose, so this cannot misroute the job. */
                    result = (INT_PTR) ShellExecuteW(dialog.hwndOwner, L"print",
                                                     (LPCWSTR) wide_path,
                                                     NULL, NULL, SW_HIDE);
                }

                if (result <= 32) {
                    gchar *message = g_strdup_printf(
                        "Windows could not print \"%s\" (error %d). A PDF is printed by "
                        "the application registered for .pdf files; install a PDF reader, "
                        "or choose the default printer so the simpler \"print\" command "
                        "can be used.",
                        path, (int) result);

                    servo_gtk_web_view_print_done(self, callback, user_data, FALSE, message);
                    g_free(message);
                } else {
                    servo_gtk_web_view_print_done(self, callback, user_data, TRUE, NULL);
                }
            }

            g_free(wide_args);
            g_free(wide_path);
            g_free(quoted);
            g_free(printer);
            GlobalUnlock(dialog.hDevNames);
        }
    }

    /* PrintDlgW may allocate these even when it returns FALSE. */
    if (dialog.hDevNames != NULL) {
        GlobalFree(dialog.hDevNames);
    }
    if (dialog.hDevMode != NULL) {
        GlobalFree(dialog.hDevMode);
    }
    if (dialog.hDC != NULL) {
        DeleteDC(dialog.hDC);
    }
}

#else

void
servo_gtk_web_view_print_pdf(ServoGtkWebView             *self,
                             const gchar                 *path,
                             ServoGtkPrintResultCallback  callback,
                             gpointer                     user_data)
{
    g_return_if_fail(SERVO_GTK_IS_WEB_VIEW(self));
    g_return_if_fail(path != NULL);

    servo_gtk_web_view_print_done(self, callback, user_data, FALSE,
                                  "this build has no printing backend");
}

#endif /* printing backends */
