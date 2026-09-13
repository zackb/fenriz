#pragma once

#include <gtk/gtk.h>

#include "compositor.hpp"
#include "inhibit.hpp"
#include "island.hpp"
#include "upower.hpp"

namespace fenriz::bar {

    // Battery, power modes, keep-awake, and the session actions: the Home tiles and footer, and the Power page.
    class PowerUi {
    public:
        PowerUi(Island& island, Power& power, Compositor& compositor, IdleInhibitor& inhibitor);
        ~PowerUi();

        PowerUi(const PowerUi&) = delete;
        PowerUi& operator=(const PowerUi&) = delete;

        // Flips keep-awake and the tile with it. False when the compositor cannot inhibit idle.
        bool toggle_awake();

    private:
        struct Action {
            const char* label;
            const char* icon;
            const char* confirm; // null runs on the first click
            void (*run)(PowerUi&);
            GtkWidget* button = nullptr;
            GtkWidget* text = nullptr;
            guint reset_id = 0;
        };

        void update();
        void announce(const Power::Battery& before, const Power::Battery& now);
        void rebuild_profiles();
        static void on_action(GtkButton* button, gpointer data);
        static void logind(const char* path, const char* iface, const char* method, GVariant* args);

        Island& island_;
        Power& power_;
        Compositor& compositor_;
        IdleInhibitor& inhibitor_;

        GtkWidget* awake_tile_ = nullptr;
        GtkWidget* mode_tile_ = nullptr;
        GtkWidget* mode_icon_ = nullptr;
        GtkWidget* mode_label_ = nullptr;
        GtkWidget* battery_footer_ = nullptr;
        GtkWidget* battery_footer_icon_ = nullptr;
        GtkWidget* battery_footer_label_ = nullptr;
        GtkWidget* battery_line_ = nullptr;
        GtkWidget* profiles_ = nullptr;
        std::vector<std::string> profiles_shown_;
        std::vector<Action> actions_;
        Power::Battery last_;
        bool seen_battery_ = false;
        bool updating_ = false;
    };

} // namespace fenriz::bar
