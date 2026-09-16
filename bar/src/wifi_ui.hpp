#pragma once

#include <gtk/gtk.h>

#include "island.hpp"
#include "network.hpp"

namespace fenriz::bar {

    // The Wi-Fi tile on the island's home page, the Wi-Fi page with its inline password entry, and connection events.
    class WifiUi {
    public:
        WifiUi(Island& island, Network& network);
        ~WifiUi();

        WifiUi(const WifiUi&) = delete;
        WifiUi& operator=(const WifiUi&) = delete;

    private:
        void update();
        void rebuild();
        GtkWidget* row(const WifiNetwork& n);
        GtkWidget* hidden_row();
        GtkWidget* password_line();
        void expand(const std::string& ssid);
        void expand_hidden(bool on);
        void submit();

        Island& island_;
        Network& net_;
        GtkWidget* tile_ = nullptr;
        GtkWidget* tile_toggle_ = nullptr;
        GtkWidget* tile_icon_ = nullptr;
        GtkWidget* tile_label_ = nullptr;
        GtkWidget* power_ = nullptr;
        GtkWidget* status_ = nullptr;
        GtkWidget* list_ = nullptr;
        GtkWidget* entry_ = nullptr; // the field to focus once a rebuild puts it in the window
        std::vector<WifiNetwork> shown_;
        std::string expanded_;    // SSID whose password entry is open
        std::string password_;    // what was typed, kept across list rebuilds
        std::string hidden_ssid_; // the name typed into the hidden-network row
        bool hidden_open_ = false;
        guint scan_id_ = 0;
        bool updating_ = false;
    };

} // namespace fenriz::bar
