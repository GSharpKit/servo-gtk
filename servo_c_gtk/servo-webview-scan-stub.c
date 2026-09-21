/*
 * Introspection-only stubs for the Servo FFI (servo/servo-webview.h).
 *
 * g-ir-scanner links a helper program against the library and RUNS it to dump
 * the GObject metadata. Linking the real servogtk3 pulls in libservoshell (the
 * Servo Rust runtime), whose jemalloc load-time constructor SIGSEGVs inside the
 * minimal scanner harness (it is fine in the full demo/CLR process — the crash
 * is specific to the scanner's dependency closure).
 *
 * Introspection never instantiates a ServoGtkWebView; it only reads the
 * GObject type/properties/signals registered in class_init, none of which call
 * these functions. So the scanner links against a twin of the library built
 * from servo-gtk3-view.c plus these no-op stubs, with no dependency on
 * libservoshell. See the `servogtk3-scan` target in CMakeLists.txt.
 *
 * This file is NEVER part of the real runtime library.
 */
#include <stddef.h>

#include "servo-webview.h"
#include "servo-pdf-server.h"

ServoWebViewHandle *
servo_webview_new(uint32_t width, uint32_t height, const char *initial_uri)
{
    (void) width; (void) height; (void) initial_uri;
    return NULL;
}

void servo_webview_free(ServoWebViewHandle *webview) { (void) webview; }

void
servo_webview_set_frame_ready_callback(ServoWebViewHandle     *webview,
                                       ServoFrameReadyCallback callback,
                                       void                   *user_data)
{
    (void) webview; (void) callback; (void) user_data;
}

void
servo_webview_set_cursor_changed_callback(ServoWebViewHandle        *webview,
                                          ServoCursorChangedCallback callback,
                                          void                      *user_data)
{
    (void) webview; (void) callback; (void) user_data;
}

void
servo_webview_set_url_changed_callback(ServoWebViewHandle     *webview,
                                       ServoUrlChangedCallback callback,
                                       void                   *user_data)
{
    (void) webview; (void) callback; (void) user_data;
}

void servo_webview_load_uri(ServoWebViewHandle *webview, const char *uri)
{
    (void) webview; (void) uri;
}

void servo_webview_reload(ServoWebViewHandle *webview) { (void) webview; }
void servo_webview_go_back(ServoWebViewHandle *webview) { (void) webview; }
void servo_webview_go_forward(ServoWebViewHandle *webview) { (void) webview; }

void
servo_webview_resize(ServoWebViewHandle *webview, uint32_t width, uint32_t height)
{
    (void) webview; (void) width; (void) height;
}

void servo_webview_pointer_move(ServoWebViewHandle *webview, double x, double y)
{
    (void) webview; (void) x; (void) y;
}

void
servo_webview_pointer_button(ServoWebViewHandle *webview,
                             uint32_t            button,
                             bool                pressed,
                             double              x,
                             double              y)
{
    (void) webview; (void) button; (void) pressed; (void) x; (void) y;
}

void servo_webview_scroll(ServoWebViewHandle *webview, double dx, double dy,
                          double x, double y)
{
    (void) webview; (void) dx; (void) dy; (void) x; (void) y;
}

void
servo_webview_key(ServoWebViewHandle *webview,
                  uint32_t            key,
                  uint32_t            unicode,
                  uint32_t            modifiers,
                  bool                pressed)
{
    (void) webview; (void) key; (void) unicode; (void) modifiers; (void) pressed;
}

void servo_webview_spin(ServoWebViewHandle *webview) { (void) webview; }

char *servo_webview_get_uri(ServoWebViewHandle *webview)
{
    (void) webview;
    return NULL;
}

void servo_string_free(char *string) { (void) string; }

void
servo_webview_evaluate_script(ServoWebViewHandle       *webview,
                              const char               *script,
                              ServoScriptResultCallback callback,
                              void                     *user_data)
{
    (void) webview; (void) script; (void) callback; (void) user_data;
}

/*
 * The same applies to the loopback PDF host: servo-gtk-pdf-server.c is part of
 * the scanned sources, so the twin needs these symbols to link. Introspection
 * only registers the GObject type; it never starts a server.
 */
ServoPdfServerHandle *servo_pdf_server_start(const char *pdfjs_dir,
                                             const char *documents_dir)
{
    (void) pdfjs_dir; (void) documents_dir;
    return NULL;
}

void servo_pdf_server_stop(ServoPdfServerHandle *server) { (void) server; }

uint16_t servo_pdf_server_port(ServoPdfServerHandle *server)
{
    (void) server;
    return 0;
}

char *servo_pdf_server_origin(ServoPdfServerHandle *server)
{
    (void) server;
    return NULL;
}

char *servo_pdf_server_url(ServoPdfServerHandle *server, const char *path)
{
    (void) server; (void) path;
    return NULL;
}

char *servo_pdf_server_add_document(ServoPdfServerHandle *server,
                                    const char *file_path, const char *name)
{
    (void) server; (void) file_path; (void) name;
    return NULL;
}

bool servo_pdf_server_remove_document(ServoPdfServerHandle *server, const char *name)
{
    (void) server; (void) name;
    return false;
}

char *servo_pdf_server_viewer_url(ServoPdfServerHandle *server,
                                  const char *document_path, const char *viewer_hash)
{
    (void) server; (void) document_path; (void) viewer_hash;
    return NULL;
}

bool servo_pdf_server_set_viewer_preferences(ServoPdfServerHandle *server,
                                             const char *preferences_json)
{
    (void) server; (void) preferences_json;
    return true;
}

void servo_pdf_server_set_viewer_toolbar_visible(ServoPdfServerHandle *server,
                                                 bool visible)
{
    (void) server; (void) visible;
}

void servo_pdf_server_set_relax_style_csp(ServoPdfServerHandle *server, bool relax)
{
    (void) server; (void) relax;
}
