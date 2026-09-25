#include "servo-gtk4-view.h"

#include "servo-webview.h"

#include <gdk/gdkkeysyms.h>

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
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/*
 * Scroll state, kept in the (previously unused) private pointer so the public
 * struct keeps its layout.
 *
 * Servo 0.5.0 has no API for the scroll offset or the scrollable extent: it
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
    /* Last pointer position, because GtkEventControllerScroll does not carry
     * one and Servo scrolls whatever sits under the given point. */
    gdouble        pointer_x;
    gdouble        pointer_y;
};

G_DEFINE_TYPE_WITH_CODE(ServoGtkWebView, servo_gtk_web_view, GTK_TYPE_DRAWING_AREA,
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

    gtk_widget_queue_draw(GTK_WIDGET(self));
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

    g_clear_object(&self->frame);

    if (self->servo != NULL) {
        servo_webview_free(self->servo);
        self->servo = NULL;
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
 * GtkDrawingArea draw func (GTK4): the widget snapshots itself into this cairo
 * context. Paint the latest Servo frame, or a neutral background if none has
 * arrived yet.
 */
static void
servo_gtk_web_view_draw(GtkDrawingArea *area,
                        cairo_t        *cr,
                        int             width,
                        int             height,
                        gpointer        user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(area);

    (void) width;
    (void) height;
    (void) user_data;

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
 * GtkDrawingArea::resize (GTK4) reports the widget's new size. The Servo
 * instance is created lazily on the first resize, when the real widget size is
 * known, and resized on subsequent ones.
 */
static void
servo_gtk_web_view_on_resize(GtkDrawingArea *area,
                             int             width,
                             int             height,
                             gpointer        user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(area);
    guint w = (guint) MAX(1, width);
    guint h = (guint) MAX(1, height);

    (void) user_data;

    if (self->servo == NULL) {
        /*
         * Pass any URI requested before allocation as the initial URL: Servo
         * creates the browsing context together with it. Issuing a separate
         * load here instead would race the context's creation and be dropped.
         */
        self->servo = servo_webview_new(w, h, self->uri);
        if (self->servo != NULL) {
            servo_webview_set_frame_ready_callback(
                self->servo, servo_gtk_web_view_on_frame_ready, self);
            servo_webview_set_cursor_changed_callback(
                self->servo, servo_gtk_web_view_on_cursor_changed, self);
            servo_webview_set_url_changed_callback(
                self->servo, servo_gtk_web_view_on_url_changed, self);
            /* Scroll geometry can only be read once there is a page to ask. */
            servo_gtk_web_view_update_scroll_polling(self);
        } else {
            /*
             * The engine could not build a rendering context at all. The
             * reason is printed by the Rust side, and it is not about graphics
             * hardware: rasterization is pure CPU and needs no GL, driver or
             * GPU.
             *
             * Nothing here needs a GL context of its own either -- Servo
             * renders into its own CPU framebuffer and this widget blits the
             * resulting frames with Cairo -- so GSK falling back to its cairo
             * renderer on a GL-less machine costs the web view nothing.
             */
            g_warning("Servo: the engine could not be created; the web view stays blank.");
        }
    } else {
        servo_webview_resize(self->servo, w, h);
    }
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
 */
static gboolean
servo_gtk_web_view_on_scroll(GtkEventControllerScroll *controller,
                             gdouble                   dx,
                             gdouble                   dy,
                             gpointer                  user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);

    (void) controller;

    if (self->servo != NULL) {
        servo_webview_scroll(self->servo, dx, dy,
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
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = servo_gtk_web_view_set_property;
    object_class->get_property = servo_gtk_web_view_get_property;
    object_class->dispose = servo_gtk_web_view_dispose;
    object_class->finalize = servo_gtk_web_view_finalize;

    properties[PROP_URI] =
        g_param_spec_string(
            "uri",
            "URI",
            "The currently loaded URI",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
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
}

static void
servo_gtk_web_view_init(ServoGtkWebView *self)
{
    self->priv = g_new0(ServoGtkWebViewPrivate, 1);

    GtkWidget *widget = GTK_WIDGET(self);

    gtk_widget_set_focusable(widget, TRUE);

    /* GTK4: the drawing area renders through a draw func rather than a vfunc. */
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(self), servo_gtk_web_view_draw, NULL, NULL);

    /* Lazily create/resize the Servo instance as the widget is allocated. */
    g_signal_connect(self, "resize", G_CALLBACK(servo_gtk_web_view_on_resize), NULL);

    /*
     * GTK4 delivers input through event controllers rather than event masks and
     * per-event widget vfuncs. Each controller is owned by the widget once
     * added, so there is nothing to explicitly unref.
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
