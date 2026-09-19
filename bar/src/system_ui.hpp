#pragma once

#include <gtk/gtk.h>

#include "island.hpp"
#include "sysstat.hpp"

namespace fenriz::bar {

    // The CPU/memory/temperature readout in the Home footer, and the System page behind it.
    class SystemUi {
    public:
        SystemUi(Island& island, SysStat& stats);

        SystemUi(const SystemUi&) = delete;
        SystemUi& operator=(const SystemUi&) = delete;

    private:
        void update();

        Island& island_;
        SysStat& stats_;
        GtkWidget* readout_ = nullptr;
        GtkWidget* cpu_readout_ = nullptr;
        GtkWidget* memory_readout_ = nullptr;
        GtkWidget* cpu_ = nullptr;
        GtkWidget* cpu_graph_ = nullptr;
        GtkWidget* memory_ = nullptr;
        GtkWidget* memory_graph_ = nullptr;
        GtkWidget* disk_ = nullptr;
        GtkWidget* disk_bar_ = nullptr;
        GtkWidget* temp_ = nullptr;
    };

} // namespace fenriz::bar
