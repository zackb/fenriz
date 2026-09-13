#pragma once

#include <gtk/gtk.h>

#include "bluetooth.hpp"
#include "island.hpp"

namespace fenriz::bar {

    // The Bluetooth tile on the island's home page, the Bluetooth page, and connection events in the pill.
    class BluetoothUi {
    public:
        BluetoothUi(Island& island, Bluetooth& bluetooth);

        BluetoothUi(const BluetoothUi&) = delete;
        BluetoothUi& operator=(const BluetoothUi&) = delete;

    private:
        void update();
        void rebuild(GtkWidget* list, const std::vector<BtDevice>& devices, std::vector<BtDevice>& shown);
        GtkWidget* row(const BtDevice& d);

        Island& island_;
        Bluetooth& bt_;
        GtkWidget* tile_ = nullptr;
        GtkWidget* tile_toggle_ = nullptr;
        GtkWidget* tile_label_ = nullptr;
        GtkWidget* tile_icon_ = nullptr;
        GtkWidget* power_ = nullptr;
        GtkWidget* off_label_ = nullptr;
        GtkWidget* paired_section_ = nullptr;
        GtkWidget* paired_ = nullptr;
        GtkWidget* nearby_section_ = nullptr;
        GtkWidget* nearby_ = nullptr;
        GtkWidget* spinner_ = nullptr;
        std::vector<BtDevice> paired_shown_;
        std::vector<BtDevice> nearby_shown_;
        bool updating_ = false;
    };

} // namespace fenriz::bar
