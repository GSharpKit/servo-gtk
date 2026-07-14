//
// Created by mkj on 23.06.2026.
//
#include <gtk/gtk.h>

#include "servo-gtk-web-view.h"

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

/* The web view navigated to a new URL: print it and reflect it in the entry. */
static void
on_web_view_uri_changed(ServoGtkWebView *web_view, const gchar *uri, gpointer user_data)
{
    GtkEntry *entry = GTK_ENTRY(user_data);

    (void) web_view;

    g_print("URL changed: %s\n", uri != NULL ? uri : "(null)");

    if (uri != NULL) {
        gtk_entry_set_text(entry, uri);
    }
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget  *window;
    GtkWidget  *box;
    GtkWidget  *label;
    GtkWidget  *url_entry;
    GtkWidget  *web_view;
    const char *initial_uri = "https://servo.org";

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Servo GTK Demo");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);

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
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    web_view = GTK_WIDGET(servo_gtk_web_view_new());
    gtk_widget_set_hexpand(web_view, TRUE);
    gtk_widget_set_vexpand(web_view, TRUE);

    /* URL bar: type an address and press Enter to navigate. */
    url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "Enter URL and press Enter");
    gtk_entry_set_text(GTK_ENTRY(url_entry), initial_uri);
    gtk_widget_set_margin_start(url_entry, 12);
    gtk_widget_set_margin_end(url_entry, 12);
    gtk_widget_set_margin_bottom(url_entry, 12);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_entry_activate), web_view);
    gtk_box_pack_start(GTK_BOX(box), url_entry, FALSE, FALSE, 0);

    /* Print and reflect URL changes reported by Servo (navigation, redirects). */
    g_signal_connect(web_view, "uri-changed", G_CALLBACK(on_web_view_uri_changed), url_entry);

    gtk_box_pack_start(GTK_BOX(box), web_view, TRUE, TRUE, 0);

    servo_gtk_web_view_load_uri(SERVO_GTK_WEB_VIEW(web_view), initial_uri);

    gtk_widget_show_all(window);
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