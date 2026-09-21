#ifndef SERVO_GTK_PDF_SERVER_H
#define SERVO_GTK_PDF_SERVER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define SERVO_GTK_TYPE_PDF_SERVER (servo_gtk_pdf_server_get_type())

G_DECLARE_FINAL_TYPE(ServoGtkPdfServer, servo_gtk_pdf_server,
                     SERVO_GTK, PDF_SERVER, GObject)

/**
 * servo_gtk_pdf_server_new:
 * @pdfjs_dir: (type filename): directory holding a PDF.js release — the one
 *   containing `build/` and `web/`
 * @documents_dir: (type filename) (nullable): directory of PDFs to expose, or
 *   %NULL to publish documents individually with
 *   servo_gtk_pdf_server_add_document()
 * @error: return location for a #GError
 *
 * Starts a loopback HTTP server that hosts the PDF.js viewer and your
 * documents.
 *
 * PDF.js cannot run from `file://` — its worker and module scripts do not load
 * there — so the viewer has to be served over a real HTTP origin. This binds
 * 127.0.0.1 on an ephemeral port and serves the viewer with the fixes Servo
 * needs (see `servo-pdf-server.h` for the details of each).
 *
 * Every URL is scoped by a capability token, so build them with
 * servo_gtk_pdf_server_get_url() or servo_gtk_pdf_server_get_viewer_url()
 * rather than by hand.
 *
 * The server runs until the object is finalized.
 *
 * Returns: (transfer full) (nullable): a new #ServoGtkPdfServer, or %NULL with
 *   @error set
 */
ServoGtkPdfServer *servo_gtk_pdf_server_new(const gchar  *pdfjs_dir,
                                            const gchar  *documents_dir,
                                            GError      **error);

/**
 * servo_gtk_pdf_server_get_origin:
 * @self: a #ServoGtkPdfServer
 *
 * The server's origin, e.g. `http://127.0.0.1:41235`. Note that this is not a
 * usable URL on its own: it carries no capability token.
 *
 * Returns: (nullable): the origin, owned by @self
 */
const gchar *servo_gtk_pdf_server_get_origin(ServoGtkPdfServer *self);

/**
 * servo_gtk_pdf_server_get_port:
 * @self: a #ServoGtkPdfServer
 *
 * Returns: the ephemeral TCP port the server listens on, or 0
 */
guint servo_gtk_pdf_server_get_port(ServoGtkPdfServer *self);

/**
 * servo_gtk_pdf_server_get_url:
 * @self: a #ServoGtkPdfServer
 * @path: a server-relative path beginning with `/`, such as
 *   `/pdfjs/web/viewer.html`
 *
 * Builds an absolute, token-scoped URL for @path.
 *
 * Returns: (transfer full) (nullable): the URL, or %NULL if @path is not
 *   server-relative
 */
gchar *servo_gtk_pdf_server_get_url(ServoGtkPdfServer *self,
                                    const gchar       *path);

/**
 * servo_gtk_pdf_server_add_document:
 * @self: a #ServoGtkPdfServer
 * @file_path: (type filename): the PDF to publish
 * @name: (nullable): the name to publish it under, or %NULL to use the file's
 *   own base name. Must be a single path segment.
 * @error: return location for a #GError
 *
 * Publishes a single file at `/documents/<name>`.
 *
 * This is the counterpart to the `documents_dir` mount for the common case of
 * "the user picked this one file": nothing but the named file becomes
 * reachable, and it is served from the path resolved here rather than from
 * anything in the URL. Publishing over an existing name replaces it.
 *
 * Returns: (transfer full) (nullable): the server-relative path
 *   (`/documents/<name>`), ready for servo_gtk_pdf_server_get_viewer_url(), or
 *   %NULL with @error set
 */
gchar *servo_gtk_pdf_server_add_document(ServoGtkPdfServer  *self,
                                         const gchar        *file_path,
                                         const gchar        *name,
                                         GError            **error);

/**
 * servo_gtk_pdf_server_remove_document:
 * @self: a #ServoGtkPdfServer
 * @name: the bare name, or the `/documents/<name>` path that
 *   servo_gtk_pdf_server_add_document() returned
 *
 * Withdraws a published document.
 *
 * Returns: %TRUE if something was withdrawn
 */
gboolean servo_gtk_pdf_server_remove_document(ServoGtkPdfServer *self,
                                              const gchar       *name);

/**
 * servo_gtk_pdf_server_get_viewer_url:
 * @self: a #ServoGtkPdfServer
 * @document_path: a path relative to the document mount (`report.pdf`), or a
 *   server-relative path starting with `/`
 * @viewer_hash: (nullable): PDF.js hash parameters without the leading `#`,
 *   e.g. `page=3&zoom=page-width`
 *
 * Builds the PDF.js viewer URL for a document — the URL to hand to
 * servo_gtk_web_view_load_uri().
 *
 * Returns: (transfer full) (nullable): the URL, or %NULL on failure
 */
gchar *servo_gtk_pdf_server_get_viewer_url(ServoGtkPdfServer *self,
                                           const gchar       *document_path,
                                           const gchar       *viewer_hash);

/**
 * servo_gtk_pdf_server_set_viewer_preferences:
 * @self: a #ServoGtkPdfServer
 * @preferences_json: (nullable): a JSON object of PDF.js option names to
 *   values, or %NULL to clear
 * @error: return location for a #GError
 *
 * Overrides PDF.js viewer settings without editing the distribution's files.
 *
 * Useful keys for an embedded viewer: `enableScripting`, `enableXfa`,
 * `sidebarViewOnLoad`, `annotationEditorMode`, `maxCanvasPixels`,
 * `defaultZoomValue`. The server applies its own defaults first and layers
 * these on top.
 *
 * Returns: %TRUE on success; %FALSE with @error set if the JSON is invalid or
 *   is not an object, in which case nothing changes
 */
gboolean servo_gtk_pdf_server_set_viewer_preferences(ServoGtkPdfServer  *self,
                                                     const gchar        *preferences_json,
                                                     GError            **error);

/**
 * servo_gtk_pdf_server_set_toolbar_visible:
 * @self: a #ServoGtkPdfServer
 * @visible: whether the PDF.js toolbar should be shown
 *
 * Shows or hides the PDF.js viewer's own toolbar. Visible by default.
 *
 * Hiding it leaves just the document, filling the window — the right shape when
 * the host application provides its own controls. Takes the secondary toolbar,
 * views manager, loading bar and sidebar with it. Presentation only: the
 * viewer's behaviour and keyboard shortcuts are unchanged.
 */
void servo_gtk_pdf_server_set_toolbar_visible(ServoGtkPdfServer *self,
                                              gboolean           visible);

/**
 * servo_gtk_pdf_server_get_toolbar_visible:
 * @self: a #ServoGtkPdfServer
 *
 * Returns: whether the PDF.js toolbar is shown
 */
gboolean servo_gtk_pdf_server_get_toolbar_visible(ServoGtkPdfServer *self);

/**
 * servo_gtk_pdf_server_set_relax_style_csp:
 * @self: a #ServoGtkPdfServer
 * @relax: whether to widen the page's own `style-src`
 *
 * Adds `'unsafe-inline'` to the `style-src` of PDF.js's own `<meta>` CSP.
 *
 * This relaxes a security control and is off by default. It exists because of
 * a Servo performance bug: with any CSP that restricts inline styles, Servo
 * re-runs a full CSP evaluation for the shadow content of
 * `<input type="range">` on every restyle, and PDF.js's viewer has five of
 * them — enough to stall the script thread so pages never finish painting.
 *
 * Only an existing `style-src` is widened; no other directive is touched.
 * Leave it off unless you have measured the stall.
 */
void servo_gtk_pdf_server_set_relax_style_csp(ServoGtkPdfServer *self,
                                              gboolean           relax);

/**
 * servo_gtk_pdf_server_get_relax_style_csp:
 * @self: a #ServoGtkPdfServer
 *
 * Returns: whether the page's `style-src` is being widened
 */
gboolean servo_gtk_pdf_server_get_relax_style_csp(ServoGtkPdfServer *self);

G_END_DECLS

#endif /* SERVO_GTK_PDF_SERVER_H */
