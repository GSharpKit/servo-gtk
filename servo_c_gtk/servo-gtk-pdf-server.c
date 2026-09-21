/*
 * GObject wrapper around the loopback PDF.js host in libservoshell
 * (servo/servo-pdf-server.h).
 *
 * The C ABI underneath is plain C, which g-ir-scanner cannot see: it has no
 * type to attach the functions to, and the symbols do not carry the namespace's
 * `servo_gtk` prefix. Wrapping it in a GObject is what puts the server in the
 * typelib, so a GIR consumer gets a normal object with properties instead of
 * having to bind `servo_pdf_server_*` by hand alongside the introspected
 * widget.
 *
 * This file uses no GTK, only GObject/GIO, so the same source is compiled into
 * both the GTK3 and GTK4 libraries.
 */
#include "servo-gtk-pdf-server.h"

#include "servo-pdf-server.h"
#include "servo-webview.h"

struct _ServoGtkPdfServer
{
    GObject parent_instance;

    ServoPdfServerHandle *server;
    gchar                *origin;
    gboolean              toolbar_visible;
    gboolean              relax_style_csp;
};

G_DEFINE_FINAL_TYPE(ServoGtkPdfServer, servo_gtk_pdf_server, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_ORIGIN,
    PROP_PORT,
    PROP_TOOLBAR_VISIBLE,
    PROP_RELAX_STYLE_CSP,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL };

/* Take ownership of a string from the FFI and hand back a GLib one. */
static gchar *
servo_gtk_pdf_server_take_string(char *owned)
{
    gchar *copy;

    if (owned == NULL) {
        return NULL;
    }

    copy = g_strdup(owned);
    servo_string_free(owned);

    return copy;
}

static void
servo_gtk_pdf_server_get_property(GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
    ServoGtkPdfServer *self = SERVO_GTK_PDF_SERVER(object);

    switch (property_id) {
    case PROP_ORIGIN:
        g_value_set_string(value, self->origin);
        break;

    case PROP_PORT:
        g_value_set_uint(value, servo_gtk_pdf_server_get_port(self));
        break;

    case PROP_TOOLBAR_VISIBLE:
        g_value_set_boolean(value, self->toolbar_visible);
        break;

    case PROP_RELAX_STYLE_CSP:
        g_value_set_boolean(value, self->relax_style_csp);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
servo_gtk_pdf_server_set_property(GObject      *object,
                                  guint         property_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    ServoGtkPdfServer *self = SERVO_GTK_PDF_SERVER(object);

    switch (property_id) {
    case PROP_TOOLBAR_VISIBLE:
        servo_gtk_pdf_server_set_toolbar_visible(self, g_value_get_boolean(value));
        break;

    case PROP_RELAX_STYLE_CSP:
        servo_gtk_pdf_server_set_relax_style_csp(self, g_value_get_boolean(value));
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
servo_gtk_pdf_server_finalize(GObject *object)
{
    ServoGtkPdfServer *self = SERVO_GTK_PDF_SERVER(object);

    /* Blocks until the acceptor thread has exited. */
    servo_pdf_server_stop(self->server);
    self->server = NULL;

    g_clear_pointer(&self->origin, g_free);

    G_OBJECT_CLASS(servo_gtk_pdf_server_parent_class)->finalize(object);
}

static void
servo_gtk_pdf_server_class_init(ServoGtkPdfServerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = servo_gtk_pdf_server_get_property;
    object_class->set_property = servo_gtk_pdf_server_set_property;
    object_class->finalize = servo_gtk_pdf_server_finalize;

    /**
     * ServoGtkPdfServer:origin:
     *
     * The server's origin, e.g. `http://127.0.0.1:41235`. Carries no
     * capability token, so it is not a usable URL on its own.
     */
    properties[PROP_ORIGIN] =
        g_param_spec_string("origin", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * ServoGtkPdfServer:port:
     *
     * The ephemeral TCP port the server listens on.
     */
    properties[PROP_PORT] =
        g_param_spec_uint("port", NULL, NULL, 0, G_MAXUINT16, 0,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * ServoGtkPdfServer:toolbar-visible:
     *
     * Whether the PDF.js viewer shows its own toolbar. Hide it when the host
     * application provides the controls itself.
     */
    properties[PROP_TOOLBAR_VISIBLE] =
        g_param_spec_boolean("toolbar-visible", NULL, NULL, TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * ServoGtkPdfServer:relax-style-csp:
     *
     * Whether to widen the page's own `style-src` with `'unsafe-inline'`. This
     * relaxes a security control and is off by default; see
     * servo_gtk_pdf_server_set_relax_style_csp().
     */
    properties[PROP_RELAX_STYLE_CSP] =
        g_param_spec_boolean("relax-style-csp", NULL, NULL, FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, properties);
}

static void
servo_gtk_pdf_server_init(ServoGtkPdfServer *self)
{
    self->toolbar_visible = TRUE;
    self->relax_style_csp = FALSE;
}

ServoGtkPdfServer *
servo_gtk_pdf_server_new(const gchar  *pdfjs_dir,
                         const gchar  *documents_dir,
                         GError      **error)
{
    g_return_val_if_fail(pdfjs_dir != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    /*
     * The FFI only reports failure as NULL, so check the obvious causes here
     * to say something useful. A directory that exists but holds no PDF.js is
     * the nastiest case: the server starts, every request 404s, and the viewer
     * is a blank page with nothing to explain it.
     */
    if (!g_file_test(pdfjs_dir, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "PDF.js directory \"%s\" does not exist", pdfjs_dir);
        return NULL;
    }
    if (documents_dir != NULL && !g_file_test(documents_dir, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "document directory \"%s\" does not exist", documents_dir);
        return NULL;
    }

    gchar    *viewer = g_build_filename(pdfjs_dir, "web", "viewer.html", NULL);
    gboolean  has_viewer = g_file_test(viewer, G_FILE_TEST_IS_REGULAR);

    g_free(viewer);

    if (!has_viewer) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "\"%s\" has no web/viewer.html; it must be the root of a "
                    "PDF.js release — the directory holding build/ and web/ — "
                    "not the web/ subdirectory itself",
                    pdfjs_dir);
        return NULL;
    }

    ServoPdfServerHandle *handle = servo_pdf_server_start(pdfjs_dir, documents_dir);

    if (handle == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "could not start the PDF server for \"%s\" (loopback socket "
                    "unavailable, or no OS entropy for the capability token)",
                    pdfjs_dir);
        return NULL;
    }

    ServoGtkPdfServer *self = g_object_new(SERVO_GTK_TYPE_PDF_SERVER, NULL);

    self->server = handle;
    self->origin = servo_gtk_pdf_server_take_string(servo_pdf_server_origin(handle));

    return self;
}

const gchar *
servo_gtk_pdf_server_get_origin(ServoGtkPdfServer *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), NULL);

    return self->origin;
}

guint
servo_gtk_pdf_server_get_port(ServoGtkPdfServer *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), 0);

    return servo_pdf_server_port(self->server);
}

gchar *
servo_gtk_pdf_server_get_url(ServoGtkPdfServer *self, const gchar *path)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), NULL);
    g_return_val_if_fail(path != NULL, NULL);

    return servo_gtk_pdf_server_take_string(servo_pdf_server_url(self->server, path));
}

gchar *
servo_gtk_pdf_server_add_document(ServoGtkPdfServer  *self,
                                  const gchar        *file_path,
                                  const gchar        *name,
                                  GError            **error)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), NULL);
    g_return_val_if_fail(file_path != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    gchar *published = servo_gtk_pdf_server_take_string(
        servo_pdf_server_add_document(self->server, file_path, name));

    if (published == NULL) {
        if (!g_file_test(file_path, G_FILE_TEST_IS_REGULAR)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "\"%s\" is not a readable file", file_path);
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "could not publish \"%s\" (the name must be a single "
                        "path segment)", file_path);
        }
        return NULL;
    }

    return published;
}

gboolean
servo_gtk_pdf_server_remove_document(ServoGtkPdfServer *self, const gchar *name)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), FALSE);
    g_return_val_if_fail(name != NULL, FALSE);

    return servo_pdf_server_remove_document(self->server, name);
}

gchar *
servo_gtk_pdf_server_get_viewer_url(ServoGtkPdfServer *self,
                                    const gchar       *document_path,
                                    const gchar       *viewer_hash)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), NULL);
    g_return_val_if_fail(document_path != NULL, NULL);

    return servo_gtk_pdf_server_take_string(
        servo_pdf_server_viewer_url(self->server, document_path, viewer_hash));
}

gboolean
servo_gtk_pdf_server_set_viewer_preferences(ServoGtkPdfServer  *self,
                                            const gchar        *preferences_json,
                                            GError            **error)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (servo_pdf_server_set_viewer_preferences(self->server, preferences_json)) {
        return TRUE;
    }

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "viewer preferences must be a JSON object");

    return FALSE;
}

void
servo_gtk_pdf_server_set_toolbar_visible(ServoGtkPdfServer *self, gboolean visible)
{
    g_return_if_fail(SERVO_GTK_IS_PDF_SERVER(self));

    visible = !!visible;
    if (self->toolbar_visible == visible) {
        return;
    }

    self->toolbar_visible = visible;
    servo_pdf_server_set_viewer_toolbar_visible(self->server, visible);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TOOLBAR_VISIBLE]);
}

gboolean
servo_gtk_pdf_server_get_toolbar_visible(ServoGtkPdfServer *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), TRUE);

    return self->toolbar_visible;
}

void
servo_gtk_pdf_server_set_relax_style_csp(ServoGtkPdfServer *self, gboolean relax)
{
    g_return_if_fail(SERVO_GTK_IS_PDF_SERVER(self));

    relax = !!relax;
    if (self->relax_style_csp == relax) {
        return;
    }

    self->relax_style_csp = relax;
    servo_pdf_server_set_relax_style_csp(self->server, relax);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RELAX_STYLE_CSP]);
}

gboolean
servo_gtk_pdf_server_get_relax_style_csp(ServoGtkPdfServer *self)
{
    g_return_val_if_fail(SERVO_GTK_IS_PDF_SERVER(self), FALSE);

    return self->relax_style_csp;
}
