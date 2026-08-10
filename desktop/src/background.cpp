#include "background.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <vector>

#include "menu.hpp"

namespace fenriz::desktop {

    Background::Background(const Config& cfg) : cfg_(cfg) {}

    Background::~Background() {
        if (monitors_handler_) {
            GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
            g_signal_handler_disconnect(monitors, monitors_handler_);
        }
        for (auto& [monitor, surface] : surfaces_) {
            gtk_widget_unparent(surface.popover);
            gtk_window_destroy(surface.window);
        }
        g_clear_object(&menu_model_);
    }

    void Background::start(GtkApplication* app) {
        app_ = app;
        menu_model_ = menu::build_model(cfg_);
        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        monitors_handler_ = g_signal_connect(monitors, "items-changed", G_CALLBACK(on_monitors_changed), this);
        sync_monitors();
    }

    void Background::on_monitors_changed(GListModel*, guint, guint, guint, gpointer data) {
        static_cast<Background*>(data)->sync_monitors();
    }

    void Background::sync_monitors() {
        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        const guint n = g_list_model_get_n_items(monitors);

        std::vector<GdkMonitor*> live;
        live.reserve(n);
        for (guint i = 0; i < n; i++) {
            // get_item returns a ref; the list keeps its own, so drop ours immediately.
            GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, i));
            g_object_unref(monitor);
            live.push_back(monitor);
            if (!surfaces_.contains(monitor))
                add_monitor(monitor);
        }

        std::vector<GdkMonitor*> gone;
        for (auto& [monitor, surface] : surfaces_)
            if (std::find(live.begin(), live.end(), monitor) == live.end())
                gone.push_back(monitor);
        for (GdkMonitor* monitor : gone)
            drop_monitor(monitor);
    }

    void Background::add_monitor(GdkMonitor* monitor) {
        const char* connector = gdk_monitor_get_connector(monitor);

        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(content, "fenriz-background");

        const std::string& path = cfg_.wallpaper_for(connector ? connector : "");
        if (!path.empty()) {
            GError* err = nullptr;
            GdkTexture* texture = gdk_texture_new_from_filename(path.c_str(), &err);
            if (texture) {
                GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
                g_object_unref(texture);
                gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
                gtk_widget_set_can_target(picture, FALSE); // right-clicks belong to the surface
                gtk_widget_set_vexpand(picture, TRUE);
                gtk_box_append(GTK_BOX(content), picture);
                g_message("wallpaper: %s -> %s", connector ? connector : "?", path.c_str());
            } else {
                g_warning("wallpaper: %s: %s", path.c_str(), err->message);
                g_error_free(err);
            }
        }

        GtkWindow* window = GTK_WINDOW(gtk_application_window_new(app_));
        gtk_layer_init_for_window(window);
        gtk_layer_set_namespace(window, "fenriz-background");
        gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_BACKGROUND);
        gtk_layer_set_monitor(window, monitor);
        for (int edge = 0; edge < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; edge++)
            gtk_layer_set_anchor(window, static_cast<GtkLayerShellEdge>(edge), TRUE);
        gtk_layer_set_exclusive_zone(window, -1);
        gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_window_set_child(window, content);

        GtkWidget* popover = gtk_popover_menu_new_from_model_full(menu_model_, GTK_POPOVER_MENU_NESTED);
        gtk_widget_set_parent(popover, content);
        gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
        gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);

        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        surfaces_[monitor] = Surface{window, content, popover};
        g_signal_connect(click, "pressed", G_CALLBACK(on_right_click), popover);
        gtk_widget_add_controller(content, GTK_EVENT_CONTROLLER(click));

        gtk_window_present(window);
    }

    void Background::on_right_click(GtkGestureClick*, int, double x, double y, gpointer data) {
        auto* popover = static_cast<GtkWidget*>(data);
        const GdkRectangle at = {static_cast<int>(x), static_cast<int>(y), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(popover), &at);
        gtk_popover_popup(GTK_POPOVER(popover));
    }

    void Background::drop_monitor(GdkMonitor* monitor) {
        auto it = surfaces_.find(monitor);
        if (it == surfaces_.end())
            return;
        gtk_widget_unparent(it->second.popover);
        gtk_window_destroy(it->second.window);
        surfaces_.erase(it);
    }

} // namespace fenriz::desktop
