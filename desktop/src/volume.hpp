#pragma once

#include <wp/wp.h>

namespace fenriz::desktop {

    // `current` stepped by `delta` percent, clamped to 0.0..1.0.
    double volume_step(double current, int delta);

    int volume_percent(double level);

    // Icon theme names from the standard audio set, picked to match the level.
    inline const char* volume_icon(int percent, bool muted) {
        if (muted || percent == 0)
            return "audio-volume-muted-symbolic";
        if (percent < 34)
            return "audio-volume-low-symbolic";
        if (percent < 67)
            return "audio-volume-medium-symbolic";
        return "audio-volume-high-symbolic";
    }

    inline const char* mic_icon(bool muted) {
        return muted ? "microphone-disabled-symbolic" : "audio-input-microphone-symbolic";
    }

    // Default sink and source volume, through wireplumber mixer-api
    class Volume {
    public:
        Volume();
        ~Volume();

        Volume(const Volume&) = delete;
        Volume& operator=(const Volume&) = delete;

        // Connects and loads the mixer plugins. False when there is no pipewire to talk to. Activation finishes on the
        // main loop.
        bool start();

        // Each returns the resulting percentage, or -1 when audio is unavailable.
        int adjust(int delta);
        int toggle_mute();
        int toggle_mic_mute();

        bool muted() const { return sink_.muted; }
        bool mic_muted() const { return source_.muted; }

    private:
        struct Node {
            const char* media_class;
            double level = -1; // last level we asked for, see STALE_AFTER_US
            bool muted = false;
            gint64 at = 0; // monotonic time of the last write
        };

        static void on_loaded(WpCore* core, GAsyncResult* res, gpointer data);
        static void on_activated(WpObject* obj, GAsyncResult* res, gpointer data);

        int apply(Node& node, int delta, bool toggle);

        WpCore* core_ = nullptr;
        WpPlugin* mixer_ = nullptr;
        WpPlugin* defaults_ = nullptr;
        int pending_ = 0;
        bool ready_ = false;
        Node sink_{"Audio/Sink"};
        Node source_{"Audio/Source"};
    };

} // namespace fenriz::desktop
