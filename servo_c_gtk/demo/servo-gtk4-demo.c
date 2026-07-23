//
// Created by mkj on 23.06.2026.
//
#include <gtk/gtk.h>

#include "servo-gtk4-view.h"

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
    GtkWidget  *web_view;
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
