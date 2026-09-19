#pragma once

#include <gtk/gtk.h>

#include <memory>

#include "island.hpp"
#include "shot.hpp"

namespace fenriz::bar {

    struct Shot;

    // `fenriz-bar shot` and the island's camera: captures, optionally annotates, then copies and/or saves.
    class ShotUi {
    public:
        ShotUi(GtkApplication* app, Island& island);
        ~ShotUi();

        ShotUi(const ShotUi&) = delete;
        ShotUi& operator=(const ShotUi&) = delete;

        // Holds `cmdline` until the shot is delivered or abandoned, so the client blocks and gets the exit status
        // and saved path. Returns the status to hand back now, which the finished shot overrides. A null `cmdline`
        // reports to the island instead.
        int take(const ShotArgs& args, GApplicationCommandLine* cmdline);

    private:
        friend struct Shot;

        void take_from_island();

        GtkApplication* app_;
        Island& island_;
        ShotArgs options_{ShotMode::Region, true, false, "", true};
        guint delay_id_ = 0;
        std::unique_ptr<Shot> shot_;
        int status_ = 0;
    };

} // namespace fenriz::bar
