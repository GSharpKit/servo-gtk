#include "servo-gtk-web-view.h"

#include "servo-webview.h"

enum {
    PROP_0,
    PROP_URI,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL };

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

static void
servo_gtk_web_view_on_cursor_changed(const char *name, gpointer user_data)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(user_data);
    GtkWidget       *widget = GTK_WIDGET(self);
    GdkWindow       *window = gtk_widget_get_window(widget);

    if (window == NULL) {
        return;
    }

    GdkCursor *cursor = gdk_cursor_new_from_name(gtk_widget_get_display(widget), name);
    gdk_window_set_cursor(window, cursor);
    if (cursor != NULL) {
        g_object_unref(cursor);
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

static gboolean
servo_gtk_web_view_draw(GtkWidget *widget, cairo_t *cr)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->frame != NULL) {
        gdk_cairo_set_source_pixbuf(cr, self->frame, 0, 0);
        cairo_paint(cr);
    } else {
        /* No frame yet: paint a neutral background. */
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
    }

    return FALSE;
}

/*
 * The Servo instance is created lazily on the first allocation, when the real
 * widget size is known, and resized on subsequent allocations.
 */
static void
servo_gtk_web_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    GTK_WIDGET_CLASS(servo_gtk_web_view_parent_class)->size_allocate(widget, allocation);

    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);
    guint width  = (guint) MAX(1, allocation->width);
    guint height = (guint) MAX(1, allocation->height);

    if (self->servo == NULL) {
        /*
         * Pass any URI requested before allocation as the initial URL: Servo
         * creates the browsing context together with it. Issuing a separate
         * load here instead would race the context's creation and be dropped.
         */
        self->servo = servo_webview_new(width, height, self->uri);
        if (self->servo != NULL) {
            servo_webview_set_frame_ready_callback(
                self->servo, servo_gtk_web_view_on_frame_ready, self);
            servo_webview_set_cursor_changed_callback(
                self->servo, servo_gtk_web_view_on_cursor_changed, self);
        }
    } else {
        servo_webview_resize(self->servo, width, height);
    }
}

static gboolean
servo_gtk_web_view_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->servo != NULL) {
        servo_webview_pointer_move(self->servo, event->x, event->y);
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_button_press(GtkWidget *widget, GdkEventButton *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    gtk_widget_grab_focus(widget);

    if (self->servo != NULL) {
        servo_webview_pointer_button(self->servo, event->button, TRUE, event->x, event->y);
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_button_release(GtkWidget *widget, GdkEventButton *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);

    if (self->servo != NULL) {
        servo_webview_pointer_button(self->servo, event->button, FALSE, event->x, event->y);
    }

    return TRUE;
}

static gboolean
servo_gtk_web_view_scroll(GtkWidget *widget, GdkEventScroll *event)
{
    ServoGtkWebView *self = SERVO_GTK_WEB_VIEW(widget);
    gdouble dx = 0.0;
    gdouble dy = 0.0;

    if (!gdk_event_get_scroll_deltas((GdkEvent *) event, &dx, &dy)) {
        switch (event->direction) {
        case GDK_SCROLL_UP:    dy = -1.0; break;
        case GDK_SCROLL_DOWN:  dy =  1.0; break;
        case GDK_SCROLL_LEFT:  dx = -1.0; break;
        case GDK_SCROLL_RIGHT: dx =  1.0; break;
        default: break;
        }
    }

    if (self->servo != NULL) {
        servo_webview_scroll(self->servo, dx, dy);
    }

    return TRUE;
}

static void
servo_gtk_web_view_class_init(ServoGtkWebViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->set_property = servo_gtk_web_view_set_property;
    object_class->get_property = servo_gtk_web_view_get_property;
    object_class->dispose = servo_gtk_web_view_dispose;
    object_class->finalize = servo_gtk_web_view_finalize;

    widget_class->draw = servo_gtk_web_view_draw;
    widget_class->size_allocate = servo_gtk_web_view_size_allocate;
    widget_class->motion_notify_event = servo_gtk_web_view_motion_notify;
    widget_class->button_press_event = servo_gtk_web_view_button_press;
    widget_class->button_release_event = servo_gtk_web_view_button_release;
    widget_class->scroll_event = servo_gtk_web_view_scroll;

    properties[PROP_URI] =
        g_param_spec_string(
            "uri",
            "URI",
            "The currently loaded URI",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
        );

    g_object_class_install_properties(object_class, N_PROPERTIES, properties);
}

static void
servo_gtk_web_view_init(ServoGtkWebView *self)
{
    GtkWidget *widget = GTK_WIDGET(self);

    gtk_widget_set_can_focus(widget, TRUE);
    gtk_widget_add_events(
        widget,
        GDK_POINTER_MOTION_MASK
        | GDK_BUTTON_PRESS_MASK
        | GDK_BUTTON_RELEASE_MASK
        | GDK_SCROLL_MASK
        | GDK_SMOOTH_SCROLL_MASK
    );

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

    /* If Servo is already up, load now; otherwise size_allocate will pick it up. */
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
