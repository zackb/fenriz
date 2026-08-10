#pragma once

#include <gtk/gtk.h>

#include <map>

#include "config.hpp"

namespace fenriz::desktop {

    // One layer-shell surface per monitor, rebuilt as monitors come and go.
    class Background {
    public:
        explicit Background(const Config& cfg);
        ~Background();

        Background(const Background&) = delete;
        Background& operator=(const Background&) = delete;

        void start(GtkApplication* app);

    private:
        struct Surface {
            GtkWindow* window;
            GtkWidget* content;
            GtkWidget* popover;
        };

        void sync_monitors();
        void add_monitor(GdkMonitor* monitor);
        void drop_monitor(GdkMonitor* monitor);

        static void on_monitors_changed(GListModel* model, guint position, guint removed, guint added, gpointer data);
        static void on_right_click(GtkGestureClick* gesture, int n_press, double x, double y, gpointer data);

        const Config& cfg_;
        GtkApplication* app_ = nullptr;
        GMenuModel* menu_model_ = nullptr;
        std::map<GdkMonitor*, Surface> surfaces_;
        gulong monitors_handler_ = 0;
    };

} // namespace fenriz::desktop
