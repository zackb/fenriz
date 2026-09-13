#pragma once

#include <gtk/gtk.h>

#include <vector>

#include "audio.hpp"
#include "brightness.hpp"
#include "island.hpp"

namespace fenriz::bar {

    // Volume and brightness sliders on the island's home page, and the Sound page behind them.
    class AudioUi {
    public:
        AudioUi(Island& island, Audio& audio, desktop::Brightness& brightness);

        AudioUi(const AudioUi&) = delete;
        AudioUi& operator=(const AudioUi&) = delete;

        // The desktop changed it (a key); nothing else signals brightness.
        void brightness_changed(int percent);

    private:
        struct Section {
            bool source;
            GtkWidget* list;
            std::vector<Audio::Device> shown;
        };

        void update();
        void rebuild(Section& section, const Audio::Endpoint& ep);

        static gboolean on_volume(GtkRange* range, GtkScrollType scroll, double value, gpointer data);
        static gboolean on_mic_volume(GtkRange* range, GtkScrollType scroll, double value, gpointer data);
        static gboolean on_brightness(GtkRange* range, GtkScrollType scroll, double value, gpointer data);
        static void on_device_toggled(GtkCheckButton* button, gpointer data);

        Island& island_;
        Audio& audio_;
        desktop::Brightness& brightness_;
        GtkWidget* volume_row_ = nullptr;
        GtkWidget* mute_ = nullptr;
        GtkWidget* volume_ = nullptr;
        GtkWidget* brightness_row_ = nullptr;
        GtkWidget* brightness_scale_ = nullptr;
        GtkWidget* mic_mute_ = nullptr;
        GtkWidget* mic_volume_ = nullptr;
        Section outputs_{false, nullptr, {}};
        Section inputs_{true, nullptr, {}};
        bool updating_ = false; // programmatic changes must not write back
    };

} // namespace fenriz::bar
