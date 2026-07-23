#include "servo-gtk4-view.h"

#include "servo-webview.h"

#include <gdk/gdkkeysyms.h>

enum {
    PROP_0,
    PROP_URI,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL };

enum {
    URI_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

G_DEFINE_TYPE(ServoGtkWebView, servo_gtk_web_view, GTK_TYPE_DRAWING_AREA)

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
        servo_webview_scroll(self->servo, dx, dy);
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
