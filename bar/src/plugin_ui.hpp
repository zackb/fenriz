#pragma once

#include <gtk/gtk.h>

#include "island.hpp"
#include "plugin.hpp"

namespace fenriz::bar {

    // Draws one plugin's state into the island: a pill notice, a Home tile and footer chip, and a page named after
    // the plugin. A plugin named after a built-in page (calendar) adds to the bottom of that page instead.
    class PluginUi {
    public:
        PluginUi(Island& island, const std::string& name, const std::string& command);

        PluginUi(const PluginUi&) = delete;
        PluginUi& operator=(const PluginUi&) = delete;

        void send(const std::string& line) { plugin_.send(line); }

    private:
        void changed(unsigned slots);
        void build_page();
        GtkWidget* block(const PluginBlock& b);
        GtkWidget* row(const PluginBlock& b);
        GtkWidget* actionable(GtkWidget* button, const std::string& action);
        void open_page();

        Island& island_;
        Plugin plugin_;
        GtkWidget* chip_ = nullptr;
        GtkWidget* chip_icon_ = nullptr;
        GtkWidget* chip_label_ = nullptr;
        GtkWidget* tile_ = nullptr;
        GtkWidget* tile_icon_ = nullptr;
        GtkWidget* tile_label_ = nullptr;
        GtkWidget* page_ = nullptr;
        bool embedded_ = false; // page_ sits under a built-in page of the same name
    };

} // namespace fenriz::bar
