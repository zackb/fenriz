#include "osd.hpp"

#include <gtk4-layer-shell.h>

namespace fenriz::desktop {

    namespace {

        constexpr int WIDTH = 280;
        constexpr int BOTTOM_MARGIN = 90;
        constexpr int ICON_SIZE = 24;
        constexpr guint LINGER_MS = 1200;
        // MUST match  .osd-pill opacity transition in theme.cpp
        constexpr guint FADE_MS = 250;

    } // namespace

    Osd::~Osd() {
        if (hide_id_)
            g_source_remove(hide_id_);
        if (window_)
            gtk_window_destroy(window_);
    }

    gboolean Osd::on_fade(gpointer data) {
        auto* osd = static_cast<Osd*>(data);
        gtk_widget_add_css_class(osd->pill_, "fading");
        osd->hide_id_ = g_timeout_add(FADE_MS, on_faded, osd);
        return G_SOURCE_REMOVE;
    }

    gboolean Osd::on_faded(gpointer data) {
        auto* osd = static_cast<Osd*>(data);
        osd->hide_id_ = 0;
        gtk_widget_set_visible(GTK_WIDGET(osd->window_), FALSE);
        return G_SOURCE_REMOVE;
    }

    void Osd::build(GtkApplication* app) {
        window_ = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, "fenriz-osd");
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        // never take keyboard
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_window_set_default_size(window_, WIDTH, -1);
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_margin(window_, GTK_LAYER_SHELL_EDGE_BOTTOM, BOTTOM_MARGIN);

        gtk_widget_add_css_class(GTK_WIDGET(window_), "fenriz-osd");

        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_add_css_class(root, "osd-pill");
        pill_ = root;

        image_ = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(image_), ICON_SIZE);
        gtk_box_append(GTK_BOX(root), image_);

        bar_ = gtk_level_bar_new_for_interval(0, 100);
        gtk_level_bar_set_mode(GTK_LEVEL_BAR(bar_), GTK_LEVEL_BAR_MODE_CONTINUOUS);
        gtk_widget_set_hexpand(bar_, TRUE);
        gtk_widget_set_valign(bar_, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(root), bar_);

        gtk_window_set_child(window_, root);
    }

    void Osd::show(GtkApplication* app, const char* icon, int percent) {
        if (!window_)
            build(app);

        gtk_image_set_from_icon_name(GTK_IMAGE(image_), icon);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bar_), CLAMP(percent, 0, 100));
        // press during the fade transitions straight back to opaque
        gtk_widget_remove_css_class(pill_, "fading");
        gtk_window_present(window_);

        if (hide_id_)
            g_source_remove(hide_id_);
        hide_id_ = g_timeout_add(LINGER_MS, on_fade, this);
    }

} // namespace fenriz::desktop
