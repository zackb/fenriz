#pragma once

#include <gtk/gtk.h>

#include <string>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "compositor.hpp"
#include "island.hpp"
#include "recorder.hpp"

namespace fenriz::bar {

    // Screen recording in the island
    class RecorderUi {
    public:
        RecorderUi(Island& island, Recorder& recorder, Audio& audio, Compositor& compositor);
        ~RecorderUi();

        RecorderUi(const RecorderUi&) = delete;
        RecorderUi& operator=(const RecorderUi&) = delete;

        // Start on the focused screen, or stop what is running. What the dot, the capsule and `fenriz-bar record` do.
        void toggle();

        // A capsule for one bar, kept current until the bar destroys it.
        GtkWidget* create_status();

    private:
        void update();
        void rebuild_sources();
        void finished(const std::string& path, bool ok);
        void notify(const std::string& path);

        static gboolean on_tick(gpointer data);

        Island& island_;
        Recorder& recorder_;
        Audio& audio_;
        Compositor& compositor_;
        std::string device_; // PipeWire node name, empty for no audio
        GtkWidget* chip_ = nullptr;
        GtkWidget* chip_label_ = nullptr;
        GtkWidget* sources_ = nullptr;
        std::vector<std::pair<std::string, std::string>> shown_; // label, node name
        std::vector<GtkWidget*> statuses_;                       // one per bar
        guint tick_id_ = 0;
    };

} // namespace fenriz::bar
