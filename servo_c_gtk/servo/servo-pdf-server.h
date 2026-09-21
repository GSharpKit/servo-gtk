/*
 * C ABI for the libservoshell loopback HTTP server — the supported way to run
 * PDF.js (or any other worker/module-script based viewer) inside a Servo
 * webview.
 *
 * Why a server at all: `file://` gives module scripts, dedicated workers,
 * fetch() and byte-range reads different (or no) behaviour, and PDF.js needs
 * all four. Serving the viewer from a real HTTP origin is what makes the
 * worker start, lets large PDFs stream in 64 KiB chunks instead of being
 * downloaded whole, and keeps the static assets in Servo's HTTP cache.
 *
 * What it does for you:
 *   - binds 127.0.0.1 on an ephemeral port (never a routable interface);
 *   - mounts the PDF.js distribution at /pdfjs and your PDFs at /documents;
 *   - serves .mjs / .js as text/javascript (the usual reason the worker dies);
 *   - answers Range requests with 206 Partial Content + Content-Range;
 *   - caches everything under /pdfjs/build immutably for a year, the rest of
 *     /pdfjs for a year, and never stores documents;
 *   - sends a viewer-sized Content-Security-Policy on the HTML it serves;
 *   - injects engine compatibility shims into the viewer HTML. PDF.js 6.x uses
 *     the TC39 Map.prototype.getOrInsert proposal, which Servo's SpiderMonkey
 *     does not expose; without the shim PDFFindController's constructor throws,
 *     PDFViewerApplication.initialize() rejects with nothing logged, and the
 *     viewer renders its toolbar and a permanently empty document area.
 *
 * Access control: a loopback port is reachable by every process on the machine,
 * so every URL is scoped by a 128-bit capability token generated from OS
 * entropy at startup and carried as the first path segment. Always build URLs
 * with servo_pdf_server_url() / servo_pdf_server_viewer_url() rather than
 * concatenating them yourself. Requests whose Host header is not a loopback
 * literal are rejected (DNS-rebinding defence).
 *
 * Unlike servo_webview_*, these functions are not tied to the GTK main thread;
 * only concurrent use of one handle is disallowed.
 *
 * Expected on-disk layout under `pdfjs_dir` (a single PDF.js release — never
 * mix build/ and web/ across versions):
 *
 *   build/pdf.mjs
 *   build/pdf.worker.mjs
 *   web/viewer.html
 *   web/viewer.mjs
 *   web/viewer.css
 *   web/locale/…
 *
 * Typical use:
 *
 *   ServoPdfServerHandle *server =
 *       servo_pdf_server_start("/usr/share/myapp/pdfjs", NULL);
 *
 *   servo_pdf_server_set_viewer_preferences(
 *       server, "{\"enableScripting\":false,\"sidebarViewOnLoad\":0}");
 *
 *   char *doc = servo_pdf_server_add_document(server, "/home/me/report.pdf", NULL);
 *   char *url = servo_pdf_server_viewer_url(server, doc, "zoom=page-width");
 *   servo_webview_load_uri(webview, url);
 *   servo_string_free(url);
 *   servo_string_free(doc);
 *
 *   ... later ...
 *   servo_pdf_server_stop(server);
 *
 * Pass a documents_dir instead when you want a whole directory browsable.
 */
#ifndef SERVO_PDF_SERVER_H
#define SERVO_PDF_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a running loopback HTTP server. */
typedef struct ServoPdfServerHandle ServoPdfServerHandle;

/*
 * Start the server.
 *
 * `pdfjs_dir`     — directory holding the PDF.js distribution (the one that
 *                   contains build/ and web/), mounted at /pdfjs.
 * `documents_dir` — directory of PDFs to expose at /documents, or NULL to
 *                   mount nothing there.
 *
 * Both directories must exist and be readable. Returns NULL on failure (missing
 * directory, socket unavailable, or no OS entropy for the capability token —
 * the server refuses to start rather than use a guessable one).
 *
 * Stop it with servo_pdf_server_stop().
 */
ServoPdfServerHandle *servo_pdf_server_start(const char *pdfjs_dir,
                                             const char *documents_dir);

/*
 * Stop the server and free the handle. Blocks until the acceptor thread has
 * exited. NULL is a no-op.
 */
void servo_pdf_server_stop(ServoPdfServerHandle *server);

/* The ephemeral port the server is listening on, or 0 for NULL. */
uint16_t servo_pdf_server_port(ServoPdfServerHandle *server);

/*
 * The server origin, e.g. "http://127.0.0.1:41235", as a newly-allocated UTF-8
 * string. Note that this is NOT a usable URL on its own — it carries no
 * capability token. Free with servo_string_free().
 */
char *servo_pdf_server_origin(ServoPdfServerHandle *server);

/*
 * Absolute, token-scoped URL for a server-relative `path` such as
 * "/pdfjs/web/viewer.html" or "/documents/report.pdf". `path` must start with
 * '/'. Returns NULL on bad input. Free with servo_string_free().
 */
char *servo_pdf_server_url(ServoPdfServerHandle *server, const char *path);

/*
 * Build the PDF.js viewer URL for a document — the URL to hand to
 * servo_webview_load_uri().
 *
 * `document_path` — a path relative to the document mount ("report.pdf",
 *                   "invoices/2026-01.pdf"), or a server-relative path
 *                   starting with '/'.
 * `viewer_hash`   — optional PDF.js hash parameters without the leading '#',
 *                   e.g. "page=3&zoom=page-width&pagemode=none". May be NULL.
 *
 * The `file` parameter is emitted same-origin and percent-encoded, which is
 * what the viewer's origin check requires. Free with servo_string_free().
 */
char *servo_pdf_server_viewer_url(ServoPdfServerHandle *server,
                                  const char           *document_path,
                                  const char           *viewer_hash);

/*
 * Publish a single file at /documents/<name>.
 *
 * This is the counterpart to the `documents_dir` mount for the common case of
 * "the user picked this one file": nothing but the named file becomes
 * reachable, and it is served from the path resolved here rather than from
 * anything in the URL. A server started with documents_dir = NULL and fed
 * through this call exposes exactly the documents you hand it, no more.
 *
 * `file_path` — the file to publish; must exist and be a regular file.
 * `name`      — the name to publish it under, or NULL to use the file's own
 *               base name. Must be a single path segment.
 *
 * Publishing over an existing name replaces it. Returns the server-relative
 * path ("/documents/<name>"), ready to hand to servo_pdf_server_viewer_url(),
 * or NULL on failure. Free with servo_string_free().
 */
char *servo_pdf_server_add_document(ServoPdfServerHandle *server,
                                    const char           *file_path,
                                    const char           *name);

/*
 * Withdraw a document published by servo_pdf_server_add_document(). `name` may
 * be the bare name or the "/documents/<name>" path that call returned. Returns
 * true if something was withdrawn.
 */
bool servo_pdf_server_remove_document(ServoPdfServerHandle *server,
                                      const char           *name);

/*
 * Add 'unsafe-inline' to the style-src of PDF.js's own <meta> CSP.
 *
 * THIS RELAXES A SECURITY CONTROL AND IS OFF BY DEFAULT. It exists because of a
 * Servo performance bug: with any CSP that restricts inline styles, Servo
 * re-runs a full CSP evaluation for the UA shadow content of
 * <input type="range"> on every restyle. PDF.js's viewer has five of them,
 * which is enough to saturate the script thread — the viewer chrome paints but
 * pages take minutes to appear, or never do. Measured on a debug build of
 * Servo 0.5.0: 3 frames / 45s and 60s+ script latency with the stock CSP,
 * versus 629 frames / 45s and instant script latency with this enabled.
 *
 * It only widens an existing style-src; no other directive is touched, and the
 * server's own CSP response header still applies. The exposure it adds is that
 * a stylesheet injected into the viewer page would no longer be blocked —
 * unattractive, but bounded on a loopback origin serving only files you
 * published. Leave it off unless you have measured the stall.
 */
void servo_pdf_server_set_relax_style_csp(ServoPdfServerHandle *server, bool relax);

/*
 * Override PDF.js viewer settings for every page served from /pdfjs, without
 * editing the distribution's files.
 *
 * `preferences_json` is a JSON object of PDF.js option names to values. The
 * server injects a small bootstrap script into the viewer HTML which applies
 * them two ways, because PDF.js splits its settings in two: preference-kind
 * options are merged into the viewer's stored preferences, and everything else
 * is pushed into PDFViewerApplicationOptions from the `webviewerloaded` event.
 * Names either side does not recognise are ignored. Pass NULL to stop injecting
 * (and restore byte-identical, cacheable HTML).
 *
 * Useful keys for an embedded, performance-sensitive viewer:
 *   "enableScripting":       false      - skip PDF JavaScript execution
 *   "enableXfa":             false      - skip XFA form rendering
 *   "sidebarViewOnLoad":     0          - no thumbnail sidebar
 *   "annotationEditorMode": -1          - disable annotation editing
 *   "textLayerMode":         1          - text layer without extra passes
 *   "defaultZoomValue":      "page-width"
 *   "maxCanvasPixels":       16777216   - cap page canvases (memory ceiling;
 *                                         Servo renders in software here, so a
 *                                         high-zoom 4K canvas is expensive)
 *   "disableAutoFetch":      true       - trade prefetch for lower memory
 *
 * Leave "disableRange", "disableStream" and "disableAutoFetch" at their false
 * defaults unless you are deliberately trading speed for memory: those three
 * are what let this server stream a large PDF in chunks.
 *
 * Returns true if accepted (or cleared), false if the JSON is invalid or is not
 * an object, in which case nothing changes.
 */
bool servo_pdf_server_set_viewer_preferences(ServoPdfServerHandle *server,
                                             const char           *preferences_json);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_PDF_SERVER_H */
