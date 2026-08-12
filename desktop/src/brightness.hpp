#pragma once

#include <gio/gio.h>

#include <string>
#include <vector>

namespace fenriz::desktop {

    // Raw sysfs level for `percent` of `max`, clamped to 1..max. Never 0.
    int dim_target(int max, int percent);

    // `current` stepped by `delta` percent of `max`, clamped to 1..max. Never 0.
    int step_target(int max, int current, int delta);

    // Backlight control through logind's Session.SetBrightness.
    class Brightness {
    public:
        Brightness();
        ~Brightness();

        Brightness(const Brightness&) = delete;
        Brightness& operator=(const Brightness&) = delete;

        // False on a machine with no internal panel
        bool available() const { return !devices_.empty(); }

        void dim_to(int percent);

        // Puts back what dim_to() saved
        void restore();

        // Steps every panel by `delta` percent of its maximum.
        // Returns the resulting percentage, or -1 when no panel.
        int adjust(int delta);

    private:
        struct Device {
            std::string name;
            int max = 0;
            int saved = -1;   // level before dimming
            int applied = -1; // level we asked for, to detect changes made behind our back
            int level = -1;   // last level adjust() asked for, see stale_after_us
        };

        void write(const Device& dev, int value);

        std::vector<Device> devices_;
        GDBusConnection* bus_ = nullptr;
        bool dimmed_ = false;
        gint64 adjusted_us_ = 0;
    };

} // namespace fenriz::desktop
