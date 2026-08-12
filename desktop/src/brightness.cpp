#include "brightness.hpp"

#include <algorithm>

namespace fenriz::desktop {

    namespace {

        constexpr const char* SYSFS = "/sys/class/backlight";

        // how long adjust() trusts its own cache over sysfs
        constexpr gint64 STALE_AFTER_US = 1000000;

        // -1 when the attribute is missing or unreadable; sysfs values are plain integers.
        int read_int(const std::string& path) {
            char* text = nullptr;
            if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr))
                return -1;
            const int value = atoi(text);
            g_free(text);
            return value;
        }

        std::string attr(const std::string& device, const char* name) {
            return std::string(SYSFS) + "/" + device + "/" + name;
        }

        void on_set_done(GObject* source, GAsyncResult* res, gpointer) {
            GError* err = nullptr;
            GVariant* out = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
            if (out)
                g_variant_unref(out);
            if (err) {
                // Expected while the session is not active
                g_warning("brightness: SetBrightness failed: %s", err->message);
                g_error_free(err);
            }
        }

    } // namespace

    int dim_target(int max, int percent) {
        if (max <= 0)
            return 0;
        return std::clamp(max * std::clamp(percent, 1, 100) / 100, 1, max);
    }

    int step_target(int max, int current, int delta) {
        if (max <= 0)
            return 0;
        int step = max * delta / 100;
        if (step == 0)
            step = (delta > 0) - (delta < 0);
        return std::clamp(current + step, 1, max);
    }

    Brightness::Brightness() {
        // Every panel the kernel exposes
        GDir* dir = g_dir_open(SYSFS, 0, nullptr);
        if (!dir)
            return;
        while (const char* name = g_dir_read_name(dir)) {
            const int max = read_int(attr(name, "max_brightness"));
            if (max > 0)
                devices_.push_back({name, max, -1, -1});
        }
        g_dir_close(dir);
    }

    Brightness::~Brightness() {
        restore();
        if (bus_) {
            g_dbus_connection_flush_sync(bus_, nullptr, nullptr);
            g_clear_object(&bus_);
        }
    }

    void Brightness::write(const Device& dev, int value) {
        if (!bus_) {
            GError* err = nullptr;
            bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
            if (!bus_) {
                g_warning("brightness: no system bus: %s", err ? err->message : "unknown");
                g_clear_error(&err);
                return;
            }
        }
        // session/auto is whichever session this process belongs to, so no session id lookup.
        g_dbus_connection_call(bus_,
                               "org.freedesktop.login1",
                               "/org/freedesktop/login1/session/auto",
                               "org.freedesktop.login1.Session",
                               "SetBrightness",
                               g_variant_new("(ssu)", "backlight", dev.name.c_str(), (guint32)value),
                               nullptr,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               nullptr,
                               on_set_done,
                               nullptr);
    }

    void Brightness::dim_to(int percent) {
        if (dimmed_)
            return;
        for (Device& dev : devices_) {
            const int current = read_int(attr(dev.name, "brightness"));
            if (current < 0)
                continue;
            const int target = dim_target(dev.max, percent);
            if (target >= current)
                continue; // already at or below the dim level, nothing to save or undo
            dev.saved = current;
            dev.applied = target;
            write(dev, target);
        }
        dimmed_ = true;
    }

    int Brightness::adjust(int delta) {
        const gint64 now = g_get_monotonic_time();
        const bool stale = now - adjusted_us_ > STALE_AFTER_US;
        adjusted_us_ = now;

        int percent = -1;
        for (Device& dev : devices_) {
            if (stale || dev.level < 0)
                dev.level = read_int(attr(dev.name, "brightness"));
            if (dev.level < 0)
                continue;

            dev.level = step_target(dev.max, dev.level, delta);
            dev.saved = -1;
            dev.applied = -1;
            write(dev, dev.level);

            if (percent < 0)
                percent = dev.level * 100 / dev.max;
        }
        return percent;
    }

    void Brightness::restore() {
        if (!dimmed_)
            return;
        dimmed_ = false;
        for (Device& dev : devices_) {
            if (dev.saved < 0)
                continue;
            const int current = read_int(attr(dev.name, "brightness"));
            if (current == dev.applied)
                write(dev, dev.saved);
            dev.saved = -1;
            dev.applied = -1;
        }
    }

} // namespace fenriz::desktop
