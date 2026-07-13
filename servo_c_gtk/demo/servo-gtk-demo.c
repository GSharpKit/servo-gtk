//
// Created by mkj on 23.06.2026.
//
#include <gtk/gtk.h>

#include "servo-gtk-web-view.h"

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget *window;
    GtkWidget *box;
    GtkWidget *label;
    GtkWidget *web_view;

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
    gtk_box_pack_start(GTK_BOX(box), web_view, TRUE, TRUE, 0);

    servo_gtk_web_view_load_uri(
        SERVO_GTK_WEB_VIEW(web_view),
        "https://servo.org"
    );

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