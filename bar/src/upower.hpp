#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    // UPower's combined battery, as its State enum reports it.
    enum class Charge { Unknown, Charging, Discharging, Full };

    Charge charge_from_upower(guint32 state);

    // "2 h 13 min", "45 min", "" for unknown (0).
    std::string format_duration(gint64 seconds);

    // A battery-level-N icon in 10% steps; UPower's own IconName only knows full/good/low/caution.
    std::string battery_icon(double percent, Charge charge);

    struct Battery {
        bool present = false;
        double percent = 0;
        Charge charge = Charge::Unknown;
        gint64 seconds_left = 0; // to empty while discharging, to full while charging; 0 when unknown
        std::string icon;
    };

    constexpr double LOW_BATTERY = 10;

    // What a battery reading is worth telling the user, given the one before it.
    struct BatteryChange {
        bool plugged = false;   // started charging
        bool unplugged = false; // started discharging
        bool low = false;       // crossed into low while discharging (or was already low at the first reading)
        bool recovered = false; // no longer low: charging, or back above the threshold
    };
    BatteryChange battery_change(const Battery& before, const Battery& now, bool first);

    // The battery and power profiles, from UPower and power-profiles-daemon on the system bus.
    class Power {
    public:
        using Battery = bar::Battery;

        Power() = default;
        ~Power();

        Power(const Power&) = delete;
        Power& operator=(const Power&) = delete;

        void start();
        void subscribe(std::function<void()> listener);

        const Battery& battery() const { return battery_; }
        const std::vector<std::string>& profiles() const { return profiles_; } // empty without the daemon
        const std::string& profile() const { return profile_; }
        void set_profile(const std::string& profile);

    private:
        void connect_profiles(const char* name, const char* path);
        void read_battery();
        void read_profiles();
        void notify();

        static void on_battery_proxy(GObject* source, GAsyncResult* res, gpointer data);
        static void on_profiles_proxy(GObject* source, GAsyncResult* res, gpointer data);

        GCancellable* cancel_ = nullptr;
        GDBusProxy* battery_proxy_ = nullptr;
        GDBusProxy* profiles_proxy_ = nullptr;
        bool legacy_profiles_ = false; // net.hadess.PowerProfiles, before the daemon moved under UPower's name
        Battery battery_;
        std::vector<std::string> profiles_;
        std::string profile_;
        std::vector<std::function<void()>> listeners_;
    };

} // namespace fenriz::bar
