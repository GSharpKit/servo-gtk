//
// Created by mkj on 23.06.2026.
//
#include <gtk/gtk.h>

#include "servo-gtk4-view.h"
#include "servo-pdf-server.h"
#include "servo-webview.h"


/*
 * Where to look for a PDF.js release when $PDFJS_DIR is not set. Each entry is
 * a directory containing build/ and web/.
 */
static const gchar *const PDFJS_SEARCH_PATH[] = {
    "/usr/share/pdf.js",
    "/usr/share/javascript/pdf",
    "/usr/share/pdfjs",
    NULL
};

/* State behind the "Open PDF" button. Owned by the window. */
typedef struct {
    ServoGtkWebView      *web_view;
    GtkWidget            *window;
    /* Started on first use and kept for the life of the window: restarting it
     * would change the origin, throwing away Servo's cache of the viewer. */
    ServoPdfServerHandle *server;
    /* Path of the PDF currently shown, so it can be spooled to a printer. */
    gchar                *current_pdf;
    GtkWidget            *print_button;
} PdfDemo;

static void
pdf_demo_free(gpointer data)
{
    PdfDemo *demo = data;

    servo_pdf_server_stop(demo->server);
    g_free(demo->current_pdf);
    g_free(demo);
}

static void
pdf_demo_error(PdfDemo *demo, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static void
pdf_demo_error(PdfDemo *demo, const gchar *format, ...)
{
    va_list args;
    va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);

    g_printerr("%s\n", message);

    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_show(dialog, GTK_WINDOW(demo->window));
    g_object_unref(dialog);
    g_free(message);
}

/* Locate a PDF.js release: $PDFJS_DIR first, then the usual system locations. */
static gchar *
find_pdfjs_dir(void)
{
    const gchar *from_env = g_getenv("PDFJS_DIR");

    if (from_env != NULL && *from_env != '\0') {
        return g_strdup(from_env);
    }

    for (gsize i = 0; PDFJS_SEARCH_PATH[i] != NULL; i++) {
        gchar *viewer = g_build_filename(PDFJS_SEARCH_PATH[i], "web", "viewer.html", NULL);
        gboolean found = g_file_test(viewer, G_FILE_TEST_IS_REGULAR);
        g_free(viewer);

        if (found) {
            return g_strdup(PDFJS_SEARCH_PATH[i]);
        }
    }

    return NULL;
}

/*
 * Bring up the loopback HTTP server that hosts PDF.js. PDF.js needs a real HTTP
 * origin — over file:// its worker and module scripts do not load — so the
 * viewer and the document are both served from 127.0.0.1 on an ephemeral port.
 */
static gboolean
ensure_pdf_server(PdfDemo *demo)
{
    if (demo->server != NULL) {
        return TRUE;
    }

    gchar *pdfjs_dir = find_pdfjs_dir();
    if (pdfjs_dir == NULL) {
        pdf_demo_error(demo,
                       "No PDF.js distribution found.\n\n"
                       "Set PDFJS_DIR to a directory containing build/ and web/ "
                       "from a PDF.js release.");
        return FALSE;
    }

    /* No documents_dir: each picked file is published individually below, so
     * nothing else on disk becomes reachable. */
    demo->server = servo_pdf_server_start(pdfjs_dir, NULL);
    if (demo->server == NULL) {
        pdf_demo_error(demo, "Could not start the PDF server for \"%s\".", pdfjs_dir);
        g_free(pdfjs_dir);
        return FALSE;
    }
    g_free(pdfjs_dir);

    /*
     * Servo re-evaluates CSP for the shadow content of <input type="range"> on
     * every restyle, and PDF.js's viewer has five of them — enough to saturate
     * the script thread, so pages never finish painting. Widening style-src
     * avoids that. It relaxes a security control, so the library leaves it off
     * by default; it is justified here because this server is on loopback and
     * only serves files the user picked. See servo-pdf-server.h.
     */
    servo_pdf_server_set_relax_style_csp(demo->server, TRUE);

    /*
     * Trim the viewer for an embedded webview. maxCanvasPixels matters most
     * here: this build rasterises pages on the CPU, so an unbounded high-zoom
     * canvas is both slow and memory-hungry.
     */
    servo_pdf_server_set_viewer_preferences(
        demo->server,
        "{"
        "  \"enableScripting\": false,"
        "  \"enableXfa\": false,"
        "  \"sidebarViewOnLoad\": 0,"
        "  \"annotationEditorMode\": -1,"
        "  \"maxCanvasPixels\": 16777216,"
        "  \"defaultZoomValue\": \"page-width\""
        "}");

    gchar *origin = servo_pdf_server_origin(demo->server);
    g_print("PDF server listening on %s\n", origin != NULL ? origin : "(unknown)");
    servo_string_free(origin);

    return TRUE;
}

/* Publish one file and point the web view at the PDF.js viewer for it. */
static void
open_pdf(PdfDemo *demo, const gchar *path)
{
    if (!ensure_pdf_server(demo)) {
        return;
    }

    gchar *document = servo_pdf_server_add_document(demo->server, path, NULL);
    if (document == NULL) {
        pdf_demo_error(demo, "Could not publish \"%s\".", path);
        return;
    }

    gchar *uri = servo_pdf_server_viewer_url(demo->server, document, "zoom=page-width");
    servo_string_free(document);

    if (uri == NULL) {
        pdf_demo_error(demo, "Could not build a viewer URL for \"%s\".", path);
        return;
    }

    servo_gtk_web_view_load_uri(demo->web_view, uri);
    servo_string_free(uri);

    /* Printing spools this same file, so keep it for the Print button. */
    g_free(demo->current_pdf);
    demo->current_pdf = g_strdup(path);
    if (demo->print_button != NULL) {
        gtk_widget_set_sensitive(demo->print_button, TRUE);
    }
}

/* Report how a print request went; cancelling is not worth a dialog. */
static void
on_print_result(ServoGtkWebView *web_view,
                gboolean         printed,
                const gchar     *error,
                gpointer         user_data)
{
    PdfDemo *demo = user_data;

    (void) web_view;

    if (error != NULL) {
        pdf_demo_error(demo, "%s", error);
    } else if (printed) {
        g_print("PDF sent to the printer\n");
    }
}

/*
 * "Print" clicked: hand the open PDF to the widget, which asks for a printer
 * and spools the file itself. The demo does not care how that happens — the
 * platform differences live in ServoGtkWebView.
 */
static void
on_print_clicked(GtkButton *button, gpointer user_data)
{
    PdfDemo *demo = user_data;

    (void) button;

    if (demo->current_pdf == NULL) {
        pdf_demo_error(demo, "Open a PDF first.");
        return;
    }

    servo_gtk_web_view_print_pdf(demo->web_view, demo->current_pdf,
                                 on_print_result, demo);
}

/*
 * The file chooser finished. `user_data` is a strong reference to the window,
 * which keeps the PdfDemo it owns alive for as long as the dialog is up.
 */
static void
on_pdf_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWindow *window = user_data;
    PdfDemo   *demo = g_object_get_data(G_OBJECT(window), "pdf-demo");
    GError    *error = NULL;
    GFile     *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);

    if (file == NULL) {
        /* Dismissing the chooser is not worth an error dialog. */
        if (error != NULL && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            g_printerr("Open PDF: %s\n", error->message);
        }
        g_clear_error(&error);
        g_object_unref(window);
        return;
    }

    gchar *path = g_file_get_path(file);
    if (path == NULL) {
        pdf_demo_error(demo, "That file is not on the local filesystem.");
    } else {
        open_pdf(demo, path);
        g_free(path);
    }

    g_object_unref(file);
    g_object_unref(window);
}

/* "Open PDF" clicked: pick a file, then hand it to PDF.js. */
static void
on_open_pdf_clicked(GtkButton *button, gpointer user_data)
{
    PdfDemo *demo = user_data;

    (void) button;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open PDF");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF documents");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_pattern(filter, "*.pdf");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog,
                         GTK_WINDOW(demo->window),
                         NULL,
                         on_pdf_chosen,
                         g_object_ref(demo->window));
    g_object_unref(dialog);
}

/* Navigate the web view to the URL typed in the entry (Enter pressed). */
static void
on_url_entry_activate(GtkEntry *entry, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(user_data);
    const gchar     *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (text == NULL || *text == '\0') {
        return;
    }

    /*
     * Servo drops URLs it can't parse, so a bare host like "example.com" would
     * silently do nothing. If no scheme was typed, assume https://.
     */
    gchar *scheme = g_uri_parse_scheme(text);
    if (scheme == NULL) {
        gchar *uri = g_strconcat("https://", text, NULL);
        servo_gtk_web_view_load_uri(web_view, uri);
        g_free(uri);
    } else {
        g_free(scheme);
        servo_gtk_web_view_load_uri(web_view, text);
    }
}

/* Result of an evaluate_script call: log the returned JSON or the error. */
static void
on_script_result(ServoGtkWebView *web_view,
                 const gchar     *result_json,
                 const gchar     *error,
                 gpointer         user_data)
{
    (void) web_view;
    (void) user_data;

    if (error != NULL) {
        g_printerr("Script error: %s\n", error);
    } else {
        g_print("Recolored h1 elements: %s\n", result_json != NULL ? result_json : "(null)");
    }
}

/*
 * A color was picked: inject a small script that sets `color` on every <h1> in
 * the page. The script returns the number of elements it touched, which is
 * delivered as JSON to on_script_result().
 */
static void
on_color_set(GtkColorButton *button, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(user_data);
    GdkRGBA          rgba;

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);

    /* gdk_rgba_to_string() yields a CSS-valid "rgb(...)"/"rgba(...)" literal
     * with no single quotes, so it embeds safely in the string below. */
    gchar *color = gdk_rgba_to_string(&rgba);
    gchar *script = g_strdup_printf(
        "(function () {"
        "  var hs = document.querySelectorAll('h1');"
        "  for (var i = 0; i < hs.length; i++) { hs[i].style.color = '%s'; }"
        "  return hs.length;"
        "})();",
        color);

    servo_gtk_web_view_evaluate_script(web_view, script, on_script_result, NULL);

    g_free(script);
    g_free(color);
}

/* The web view navigated to a new URL: print it and reflect it in the entry. */
static void
on_web_view_uri_changed(ServoGtkWebView *web_view, const gchar *uri, gpointer user_data)
{
    GtkEditable *entry = GTK_EDITABLE(user_data);

    (void) web_view;

    g_print("URL changed: %s\n", uri != NULL ? uri : "(null)");

    if (uri != NULL) {
        gtk_editable_set_text(entry, uri);
    }
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget  *window;
    GtkWidget  *box;
    GtkWidget  *label;
    GtkWidget  *url_bar;
    GtkWidget  *url_entry;
    GtkWidget  *color_button;
    GtkWidget  *pdf_button;
    GtkWidget  *print_button;
    GtkWidget  *web_view;
    PdfDemo    *pdf_demo;
    const char *initial_uri = "https://servo.org";

    (void) user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Servo GTK Demo");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), box);

    label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_markup(
        GTK_LABEL(label),
        "<b>Servo GTK Demo</b>\n"
        "This demo uses libservoshell and ServoGtkWebView."
    );
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 12);
    gtk_box_append(GTK_BOX(box), label);

    web_view = GTK_WIDGET(servo_gtk_web_view_new());
    gtk_widget_set_hexpand(web_view, TRUE);
    gtk_widget_set_vexpand(web_view, TRUE);

    /* URL bar: type an address and press Enter to navigate; a color button on
     * the same row recolors every <h1> in the page. */
    url_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(url_bar, 12);
    gtk_widget_set_margin_end(url_bar, 12);
    gtk_widget_set_margin_bottom(url_bar, 12);

    url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "Enter URL and press Enter");
    gtk_editable_set_text(GTK_EDITABLE(url_entry), initial_uri);
    gtk_widget_set_hexpand(url_entry, TRUE);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_entry_activate), web_view);
    gtk_box_append(GTK_BOX(url_bar), url_entry);

    color_button = gtk_color_button_new();
    gtk_widget_set_tooltip_text(color_button, "Set the color of all <h1> headings");
    g_signal_connect(color_button, "color-set", G_CALLBACK(on_color_set), web_view);
    gtk_box_append(GTK_BOX(url_bar), color_button);

    /* "Open PDF" serves the picked file, and PDF.js itself, from a loopback
     * HTTP server; the state it needs lives as long as the window. */
    pdf_demo = g_new0(PdfDemo, 1);
    pdf_demo->web_view = SERVO_GTK_WEB_VIEW(web_view);
    pdf_demo->window = window;
    g_object_set_data_full(G_OBJECT(window), "pdf-demo", pdf_demo, pdf_demo_free);

    pdf_button = gtk_button_new_with_mnemonic("Open _PDF");
    gtk_widget_set_tooltip_text(pdf_button, "Open a PDF file in the PDF.js viewer");
    g_signal_connect(pdf_button, "clicked", G_CALLBACK(on_open_pdf_clicked), pdf_demo);
    gtk_box_append(GTK_BOX(url_bar), pdf_button);

    /* Only offered where the widget has a printing backend, and insensitive
     * until there is a document to print. */
    if (servo_gtk_web_view_can_print()) {
        print_button = gtk_button_new_with_mnemonic("P_rint");
        gtk_widget_set_tooltip_text(print_button,
                                    "Send the open PDF straight to a printer, unmodified");
        gtk_widget_set_sensitive(print_button, FALSE);
        g_signal_connect(print_button, "clicked", G_CALLBACK(on_print_clicked), pdf_demo);
        gtk_box_append(GTK_BOX(url_bar), print_button);
        pdf_demo->print_button = print_button;
    }

    gtk_box_append(GTK_BOX(box), url_bar);

    /* Print and reflect URL changes reported by Servo (navigation, redirects). */
    g_signal_connect(web_view, "uri-changed", G_CALLBACK(on_web_view_uri_changed), url_entry);

    gtk_box_append(GTK_BOX(box), web_view);

    servo_gtk_web_view_load_uri(SERVO_GTK_WEB_VIEW(web_view), initial_uri);

    gtk_window_present(GTK_WINDOW(window));
}

int
main(int argc, char **argv)
{
    GtkApplication *app;
    int status;

    app = gtk_application_new(
        "org.example.ServoGtk4Demo",
        G_APPLICATION_DEFAULT_FLAGS
    );

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);

    return status;
}
