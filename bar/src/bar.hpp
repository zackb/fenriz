#pragma once

#include <gtk/gtk.h>

#include <map>
#include <string>
#include <vector>

#include "audio.hpp"
#include "bluetooth.hpp"
#include "compositor.hpp"
#include "island.hpp"
#include "mpris.hpp"
#include "network.hpp"
#include "tray_ui.hpp"
#include "upower.hpp"

namespace fenriz::bar {

    // One bar surface per monitor, rebuilt as monitors come and go.
    class Bar {
    public:
        Bar(Compositor& compositor,
            Island& island,
            Audio& audio,
            Mpris& mpris,
            Power& power,
            Bluetooth& bluetooth,
            Network& network,
            TrayUi& tray);
        ~Bar();

        Bar(const Bar&) = delete;
        Bar& operator=(const Bar&) = delete;

        void start(GtkApplication* app);
        void update(const CompositorState& state);

        // The monitor for a connector name, or null.
        GdkMonitor* monitor_for(const std::string& connector) const;

    private:
        struct Surface {
            GtkWindow* window;
            GtkWidget* left;
            GtkWidget* workspaces;
            GtkWidget* window_box;
            GtkWidget* window_icon;
            GtkWidget* window_title;
            std::string built; // what the workspace buttons were last built from
            GtkWidget* media;
            GtkWidget* media_icon;
            GtkWidget* media_label;
            GtkWidget* volume;
            GtkWidget* bluetooth;
            GtkWidget* network;
            GtkWidget* battery;
            GtkWidget* battery_icon;
            GtkWidget* battery_label;
        };

        void sync_monitors();
        void add_monitor(GdkMonitor* monitor);
        void drop_monitor(GdkMonitor* monitor);
        void update_surface(GdkMonitor* monitor, Surface& surface, const CompositorState& state);
        void update_media(Surface& surface);
        void update_volume(Surface& surface);
        void update_battery(Surface& surface);
        void update_bluetooth(Surface& surface);
        void update_network(Surface& surface);
        // A capsule button that opens the island on `page`, on the screen it was clicked on.
        GtkWidget* page_button(GdkMonitor* monitor, const char* page, GtkWidget* child, const char* css_class);

        // The left cluster's layout: workspaces at full width, then title and song sharing what is left before the
        // island.
        static void measure_left(GtkWidget* left,
                                 GtkOrientation orientation,
                                 int for_size,
                                 int* minimum,
                                 int* natural,
                                 int* minimum_baseline,
                                 int* natural_baseline);
        static void allocate_left(GtkWidget* left, int width, int height, int baseline);

        static void on_monitors_changed(GListModel* model, guint position, guint removed, guint added, gpointer data);
        static void on_workspace_clicked(GtkButton* button, gpointer data);
        static gboolean on_scroll(GtkEventControllerScroll* scroll, double dx, double dy, gpointer data);
        static gboolean on_volume_scroll(GtkEventControllerScroll* scroll, double dx, double dy, gpointer data);

        Compositor& compositor_;
        Island& island_;
        Audio& audio_;
        Mpris& mpris_;
        Power& power_;
        Bluetooth& bluetooth_;
        Network& network_;
        TrayUi& tray_;
        GtkApplication* app_ = nullptr;
        std::map<GdkMonitor*, Surface> surfaces_;
        gulong monitors_handler_ = 0;
    };

} // namespace fenriz::bar
