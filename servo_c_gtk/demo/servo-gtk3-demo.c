//
// Created by mkj on 23.06.2026.
//
#include <gtk/gtk.h>

#include "servo-gtk3-view.h"
#include "servo-gtk-pdf-server.h"


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

/*
 * Per-window state, shared by every tab in it. Owned by the window.
 *
 * The PDF server is started on first use and kept for the life of the window:
 * restarting it would change the origin, throwing away Servo's cache of the
 * viewer. It is per window rather than per tab for the same reason.
 */
typedef struct {
    GtkWidget            *window;
    GtkWidget            *notebook;
    ServoGtkPdfServer    *server;
    /* Numbers tabs in the log, so a create can be matched to its free. */
    guint                 next_tab_id;
} DemoWindow;

/*
 * Per-tab state. Owned by the tab's web view (g_object_set_data_full), so it
 * lives exactly as long as the web view does -- including past the tab being
 * closed, while one of the widget's asynchronous calls (script result, print
 * result) still holds a reference to the view. The widget pointers are weak:
 * they belong to the page and may be gone before the web view is.
 */
typedef struct {
    DemoWindow           *win;
    ServoGtkWebView      *web_view;
    guint                 id;
    GtkWidget            *page;
    GtkWidget            *tab_label;
    GtkWidget            *url_entry;
    GtkWidget            *print_button;
    /* Path of the PDF currently shown, so it can be spooled to a printer. */
    gchar                *current_pdf;
} DemoTab;

/*
 * Web views alive in this process. Every one owns a Servo webview, painter,
 * WebRender instance and swgl framebuffer, so this is the number to watch
 * when testing that closing a tab really frees its engine.
 */
static guint live_web_views = 0;

static void
demo_window_free(gpointer data)
{
    DemoWindow *win = data;

    g_clear_object(&win->server);
    g_free(win);
}

static void
demo_window_error(DemoWindow *win, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static void
demo_window_error(DemoWindow *win, const gchar *format, ...)
{
    va_list args;
    va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);

    g_printerr("%s\n", message);

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win->window),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_CLOSE,
                                               "%s",
                                               message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(message);
}

/* Whether `dir` looks like the root of a PDF.js release. */
static gboolean
is_pdfjs_dir(const gchar *dir)
{
    gchar    *viewer = g_build_filename(dir, "web", "viewer.html", NULL);
    gboolean  found = g_file_test(viewer, G_FILE_TEST_IS_REGULAR);

    g_free(viewer);

    return found;
}

/*
 * Locate a PDF.js release: $PDFJS_DIR first, then the usual system locations.
 *
 * An explicit $PDFJS_DIR is handed back as-is, even if it looks wrong, so that
 * ServoGtkPdfServer can say exactly what is wrong with it. The candidates we
 * guess at are filtered, since "not a PDF.js directory" is the expected answer
 * for most of them.
 */
static gchar *
find_pdfjs_dir(void)
{
    const gchar *from_env = g_getenv("PDFJS_DIR");

    if (from_env != NULL && *from_env != '\0') {
        return g_strdup(from_env);
    }

    for (gsize i = 0; PDFJS_SEARCH_PATH[i] != NULL; i++) {
        if (is_pdfjs_dir(PDFJS_SEARCH_PATH[i])) {
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
ensure_pdf_server(DemoWindow *demo)
{
    if (demo->server != NULL) {
        return TRUE;
    }

    gchar *pdfjs_dir = find_pdfjs_dir();
    if (pdfjs_dir == NULL) {
        demo_window_error(demo,
                          "No PDF.js distribution found.\n\n"
                          "Set PDFJS_DIR to the root of a PDF.js release: the "
                          "directory containing build/ and web/.");
        return FALSE;
    }

    /* No documents_dir: each picked file is published individually below, so
     * nothing else on disk becomes reachable. The server validates the
     * directory and reports precisely what is wrong with it. */
    GError *error = NULL;

    demo->server = servo_gtk_pdf_server_new(pdfjs_dir, NULL, &error);
    g_free(pdfjs_dir);

    if (demo->server == NULL) {
        demo_window_error(demo, "%s", error->message);
        g_clear_error(&error);
        return FALSE;
    }

    /*
     * Servo re-evaluates CSP for the shadow content of <input type="range"> on
     * every restyle, and PDF.js's viewer has five of them — enough to saturate
     * the script thread, so pages never finish painting. Widening style-src
     * avoids that. It relaxes a security control, so the library leaves it off
     * by default; it is justified here because this server is on loopback and
     * only serves files the user picked. See servo-gtk-pdf-server.h.
     */
    servo_gtk_pdf_server_set_relax_style_csp(demo->server, TRUE);

    /*
     * Trim the viewer for an embedded webview. maxCanvasPixels matters most
     * here: this build rasterises pages on the CPU, so an unbounded high-zoom
     * canvas is both slow and memory-hungry.
     */
    if (!servo_gtk_pdf_server_set_viewer_preferences(
            demo->server,
            "{"
            "  \"enableScripting\": false,"
            "  \"enableXfa\": false,"
            "  \"sidebarViewOnLoad\": 0,"
            "  \"annotationEditorMode\": -1,"
            "  \"maxCanvasPixels\": 16777216,"
            "  \"defaultZoomValue\": \"page-width\""
            "}",
            &error)) {
        /* Not fatal — the viewer simply keeps its own defaults. */
        g_warning("viewer preferences rejected: %s", error->message);
        g_clear_error(&error);
    }

    /* PDF.js keeps its own toolbar here. Pass FALSE for a chromeless viewer,
     * which suits an application that provides the controls itself. */
    servo_gtk_pdf_server_set_toolbar_visible(demo->server, TRUE);

    /* Owned by the server object, so nothing to free. */
    g_print("PDF server listening on %s\n",
            servo_gtk_pdf_server_get_origin(demo->server));

    return TRUE;
}

/* Publish one file and point the tab's web view at the PDF.js viewer for it. */
static void
open_pdf(DemoTab *tab, const gchar *path)
{
    DemoWindow *win = tab->win;

    if (!ensure_pdf_server(win)) {
        return;
    }

    GError *error = NULL;
    gchar  *document = servo_gtk_pdf_server_add_document(win->server, path, NULL, &error);

    if (document == NULL) {
        demo_window_error(win, "%s", error->message);
        g_clear_error(&error);
        return;
    }

    gchar *uri = servo_gtk_pdf_server_get_viewer_url(win->server, document,
                                                     "zoom=page-width");
    g_free(document);

    if (uri == NULL) {
        demo_window_error(win, "Could not build a viewer URL for \"%s\".", path);
        return;
    }

    servo_gtk_web_view_load_uri(tab->web_view, uri);
    g_free(uri);

    /* Printing spools this same file, so keep it for the Print button. */
    g_free(tab->current_pdf);
    tab->current_pdf = g_strdup(path);
    if (tab->print_button != NULL) {
        gtk_widget_set_sensitive(tab->print_button, TRUE);
    }
}

/* Report how a print request went; cancelling is not worth a dialog. */
static void
on_print_result(ServoGtkWebView *web_view,
                gboolean         printed,
                const gchar     *error,
                gpointer         user_data)
{
    DemoTab *tab = user_data;

    (void) web_view;

    if (error != NULL) {
        demo_window_error(tab->win, "%s", error);
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
    DemoTab *tab = user_data;

    (void) button;

    if (tab->current_pdf == NULL) {
        demo_window_error(tab->win, "Open a PDF first.");
        return;
    }

    /* The widget holds a reference to the web view until the result is
     * delivered, and the web view owns `tab`, so `tab` outlives the call. */
    servo_gtk_web_view_print_pdf(tab->web_view, tab->current_pdf,
                                 on_print_result, tab);
}

/* "Open PDF" clicked: pick a file, then hand it to PDF.js. */
static void
on_open_pdf_clicked(GtkButton *button, gpointer user_data)
{
    DemoTab *tab = user_data;

    (void) button;

    GtkWidget *chooser = gtk_file_chooser_dialog_new("Open PDF",
                                                     GTK_WINDOW(tab->win->window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF documents");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));

        gtk_widget_hide(chooser);
        if (path != NULL) {
            open_pdf(tab, path);
            g_free(path);
        }
    }

    gtk_widget_destroy(chooser);
}

/* Navigate the web view to the URL typed in the entry (Enter pressed). */
static void
on_url_entry_activate(GtkEntry *entry, gpointer user_data)
{
    ServoGtkWebView *web_view = SERVO_GTK_WEB_VIEW(user_data);
    const gchar     *text = gtk_entry_get_text(entry);

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

/*
 * Short text for a tab: the host of `uri`, or the URI itself for schemes that
 * have none (about:, data:, file:).
 */
static gchar *
tab_title_for_uri(const gchar *uri)
{
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);

    if (parsed != NULL) {
        const gchar *host = g_uri_get_host(parsed);
        gchar       *title = (host != NULL && *host != '\0') ? g_strdup(host) : NULL;

        g_uri_unref(parsed);
        if (title != NULL) {
            return title;
        }
    }

    return g_strdup(uri);
}

/* The web view navigated to a new URL: print it and reflect it in the tab. */
static void
on_web_view_uri_changed(ServoGtkWebView *web_view, const gchar *uri, gpointer user_data)
{
    DemoTab *tab = user_data;

    (void) web_view;

    g_print("tab %u: URL changed: %s\n", tab->id, uri != NULL ? uri : "(null)");

    if (uri == NULL) {
        return;
    }

    if (tab->url_entry != NULL) {
        gtk_entry_set_text(GTK_ENTRY(tab->url_entry), uri);
    }
    if (tab->tab_label != NULL) {
        gchar *title = tab_title_for_uri(uri);

        gtk_label_set_text(GTK_LABEL(tab->tab_label), title);
        gtk_widget_set_tooltip_text(tab->tab_label, uri);
        g_free(title);
    }
}

/* ------------------------------------------------------------------------
 * Tabs
 *
 * Every tab owns one ServoGtkWebView, and with it one Servo webview, painter,
 * WebRender instance and CPU framebuffer. Closing a tab is what exercises the
 * teardown path: the web view's dispose calls servo_webview_free(). The log
 * lines below mark each step, and the live count must drop back to what it
 * was before the tab was opened.
 * ------------------------------------------------------------------------ */

/* Last reference to the web view gone: its DemoTab goes with it. */
static void
demo_tab_free(gpointer data)
{
    DemoTab *tab = data;

    live_web_views--;
    g_print("tab %u: web view finalized, DemoTab freed (%u web views alive)\n",
            tab->id, live_web_views);

    /* The tab label belongs to the notebook, not to the page, and can outlive
     * the web view; its weak pointer must not write into a freed DemoTab. */
    if (tab->tab_label != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(tab->tab_label), (gpointer *) &tab->tab_label);
    }
    if (tab->url_entry != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(tab->url_entry), (gpointer *) &tab->url_entry);
    }
    if (tab->print_button != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(tab->print_button), (gpointer *) &tab->print_button);
    }

    g_free(tab->current_pdf);
    g_free(tab);
}

/* Emitted from the web view's dispose, which is where it frees its engine. */
static void
on_web_view_destroy(GtkWidget *widget, gpointer user_data)
{
    DemoTab *tab = user_data;

    (void) widget;

    g_print("tab %u: web view disposed, Servo webview freed\n", tab->id);
}

static void
close_tab(DemoTab *tab)
{
    /* Copied out first: removing the page usually finalizes the web view on
     * the spot, and `tab` is freed with it. */
    DemoWindow  *win = tab->win;
    GtkNotebook *notebook = GTK_NOTEBOOK(win->notebook);
    gint         page_num = gtk_notebook_page_num(notebook, tab->page);

    if (page_num < 0) {
        return;
    }

    g_print("tab %u: closing\n", tab->id);
    /*
     * gtk_widget_destroy() rather than gtk_notebook_remove_page(): it is what
     * a GTK3 application does, and it runs the web view's dispose -- and so
     * servo_webview_free() -- even while an asynchronous call still holds a
     * reference to the view. The DemoTab then lives on until that reference
     * is dropped, which is the case the widget has to get right.
     */
    gtk_widget_destroy(tab->page);
    /*
     * The last web view takes the shared Servo instance down with it, and
     * Servo cannot be started twice in one process, so a window with no tabs
     * could never get a working one again. Close it instead, as browsers do.
     */
    if (gtk_notebook_get_n_pages(notebook) == 0) {
        gtk_window_close(GTK_WINDOW(win->window));
    }
}

static void
on_close_tab_clicked(GtkButton *button, gpointer user_data)
{
    (void) button;

    close_tab(user_data);
}

/* Build one tab: URL bar, the web view in a scrolled window, and its label. */
static DemoTab *
add_tab(DemoWindow *win, const gchar *uri)
{
    GtkWidget  *page;
    GtkWidget  *url_bar;
    GtkWidget  *url_entry;
    GtkWidget  *color_button;
    GtkWidget  *pdf_button;
    GtkWidget  *print_button;
    GtkWidget  *web_view;
    GtkWidget  *scrolled;
    GtkWidget  *tab_box;
    GtkWidget  *tab_label;
    GtkWidget  *close_button;
    DemoTab    *tab;
    gint        page_num;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    web_view = GTK_WIDGET(servo_gtk_web_view_new());
    gtk_widget_set_hexpand(web_view, TRUE);
    gtk_widget_set_vexpand(web_view, TRUE);

    tab = g_new0(DemoTab, 1);
    tab->win = win;
    tab->web_view = SERVO_GTK_WEB_VIEW(web_view);
    tab->id = ++win->next_tab_id;
    tab->page = page;
    g_object_set_data_full(G_OBJECT(web_view), "demo-tab", tab, demo_tab_free);
    /* Borrowed: the page holds the web view, which owns `tab`. */
    g_object_set_data(G_OBJECT(page), "demo-tab", tab);
    g_signal_connect(web_view, "destroy", G_CALLBACK(on_web_view_destroy), tab);

    live_web_views++;
    g_print("tab %u: created (%u web views alive)\n", tab->id, live_web_views);

    /* URL bar: type an address and press Enter to navigate; a color button on
     * the same row recolors every <h1> in the page. */
    url_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(url_bar, 12);
    gtk_widget_set_margin_end(url_bar, 12);
    gtk_widget_set_margin_top(url_bar, 6);
    gtk_widget_set_margin_bottom(url_bar, 6);

    url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "Enter URL and press Enter");
    gtk_entry_set_text(GTK_ENTRY(url_entry), uri);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_entry_activate), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), url_entry, TRUE, TRUE, 0);
    tab->url_entry = url_entry;
    g_object_add_weak_pointer(G_OBJECT(url_entry), (gpointer *) &tab->url_entry);

    color_button = gtk_color_button_new();
    gtk_widget_set_tooltip_text(color_button, "Set the color of all <h1> headings");
    g_signal_connect(color_button, "color-set", G_CALLBACK(on_color_set), web_view);
    gtk_box_pack_start(GTK_BOX(url_bar), color_button, FALSE, FALSE, 0);

    /* "Open PDF" serves the picked file, and PDF.js itself, from the window's
     * loopback HTTP server. */
    pdf_button = gtk_button_new_with_mnemonic("Open _PDF");
    gtk_widget_set_tooltip_text(pdf_button, "Open a PDF file in the PDF.js viewer");
    g_signal_connect(pdf_button, "clicked", G_CALLBACK(on_open_pdf_clicked), tab);
    gtk_box_pack_start(GTK_BOX(url_bar), pdf_button, FALSE, FALSE, 0);

    /* Only offered where the widget has a printing backend, and insensitive
     * until there is a document to print. */
    if (servo_gtk_web_view_can_print()) {
        print_button = gtk_button_new_with_mnemonic("P_rint");
        gtk_widget_set_tooltip_text(print_button,
                                    "Send the open PDF straight to a printer, unmodified");
        gtk_widget_set_sensitive(print_button, FALSE);
        g_signal_connect(print_button, "clicked", G_CALLBACK(on_print_clicked), tab);
        gtk_box_pack_start(GTK_BOX(url_bar), print_button, FALSE, FALSE, 0);
        tab->print_button = print_button;
        g_object_add_weak_pointer(G_OBJECT(print_button), (gpointer *) &tab->print_button);
    }

    gtk_box_pack_start(GTK_BOX(page), url_bar, FALSE, FALSE, 0);

    /* Print and reflect URL changes reported by Servo (navigation, redirects). */
    g_signal_connect(web_view, "uri-changed", G_CALLBACK(on_web_view_uri_changed), tab);

    /*
     * ServoGtkWebView implements GtkScrollable, so putting it in a
     * GtkScrolledWindow is all that is needed to get real scrollbars: the
     * widget keeps the adjustments in step with whatever the page scrolls.
     */
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), web_view);
    gtk_box_pack_start(GTK_BOX(page), scrolled, TRUE, TRUE, 0);

    /* Tab label: the page's host, and a button that closes the tab. */
    tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    gchar *title = tab_title_for_uri(uri);
    tab_label = gtk_label_new(title);
    g_free(title);
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 24);
    gtk_widget_set_tooltip_text(tab_label, uri);
    gtk_box_pack_start(GTK_BOX(tab_box), tab_label, TRUE, TRUE, 0);
    tab->tab_label = tab_label;
    g_object_add_weak_pointer(G_OBJECT(tab_label), (gpointer *) &tab->tab_label);

    close_button = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close_button, "Close tab (Ctrl+W)");
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_tab_clicked), tab);
    gtk_box_pack_start(GTK_BOX(tab_box), close_button, FALSE, FALSE, 0);
    gtk_widget_show_all(tab_box);

    /* GtkNotebook in GTK3 refuses to switch to a page that is not visible. */
    gtk_widget_show_all(page);

    page_num = gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook), page, tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(win->notebook), page, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook), page_num);

    /*
     * Servo is created on the web view's first size allocation, so an initial
     * URI set here becomes the browsing context's first URL. A tab that is
     * never shown never gets an engine at all.
     */
    servo_gtk_web_view_load_uri(SERVO_GTK_WEB_VIEW(web_view), uri);

    gtk_widget_grab_focus(url_entry);

    return tab;
}

/* The tab in front, or NULL if every tab has been closed. */
static DemoTab *
current_tab(DemoWindow *win)
{
    GtkNotebook *notebook = GTK_NOTEBOOK(win->notebook);
    gint         page_num = gtk_notebook_get_current_page(notebook);

    if (page_num < 0) {
        return NULL;
    }

    return g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(notebook, page_num)),
                             "demo-tab");
}

static void
on_new_tab_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void) action;
    (void) parameter;

    add_tab(user_data, "https://servo.org");
}

static void
on_close_tab_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    DemoTab *tab = current_tab(user_data);

    (void) action;
    (void) parameter;

    if (tab != NULL) {
        close_tab(tab);
    }
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget  *window;
    GtkWidget  *box;
    GtkWidget  *label;
    GtkWidget  *notebook;
    GtkWidget  *new_tab_button;
    DemoWindow *win;

    static const GActionEntry win_actions[] = {
        { .name = "new-tab",   .activate = on_new_tab_action },
        { .name = "close-tab", .activate = on_close_tab_action },
    };
    static const gchar *const new_tab_accels[]   = { "<Control>t", NULL };
    static const gchar *const close_tab_accels[] = { "<Control>w", NULL };

    (void) user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Servo GTK Demo");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

    win = g_new0(DemoWindow, 1);
    win->window = window;
    g_object_set_data_full(G_OBJECT(window), "demo-window", win, demo_window_free);

    g_action_map_add_action_entries(G_ACTION_MAP(window), win_actions,
                                    G_N_ELEMENTS(win_actions), win);
    gtk_application_set_accels_for_action(app, "win.new-tab", new_tab_accels);
    gtk_application_set_accels_for_action(app, "win.close-tab", close_tab_accels);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);

    label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_markup(
        GTK_LABEL(label),
        "<b>Servo GTK Demo</b>\n"
        "This demo uses libservoshell and ServoGtkWebView. "
        "Each tab is its own web view; Ctrl+T opens one, Ctrl+W closes it."
    );
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 12);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(box), notebook, TRUE, TRUE, 0);
    win->notebook = notebook;

    new_tab_button = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(new_tab_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(new_tab_button, "New tab (Ctrl+T)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(new_tab_button), "win.new-tab");
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), new_tab_button, GTK_PACK_END);

    gtk_widget_show_all(window);

    add_tab(win, "https://servo.org");
}

int
main(int argc, char **argv)
{
    GtkApplication *app;
    int status;

    app = gtk_application_new(
        "org.example.ServoGtkDemo",
        G_APPLICATION_DEFAULT_FLAGS
    );

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);

    return status;
}