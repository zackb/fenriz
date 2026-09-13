#include "upower.hpp"

#include <algorithm>

namespace fenriz::bar {

    namespace {

        constexpr const char* PROFILES_NAME = "org.freedesktop.UPower.PowerProfiles";
        constexpr const char* PROFILES_PATH = "/org/freedesktop/UPower/PowerProfiles";
        constexpr const char* LEGACY_PROFILES_NAME = "net.hadess.PowerProfiles";
        constexpr const char* LEGACY_PROFILES_PATH = "/net/hadess/PowerProfiles";

        // UPower's line-power device misses unplug uevents on some machines, so AC is polled from sysfs.
        constexpr guint AC_POLL_SECONDS = 2;
        constexpr const char* POWER_SUPPLY = "/sys/class/power_supply";

        // The first Mains supply's online file; USB-C ports are type USB and can report phantom supplies.
        std::string find_ac_online_path() {
            GDir* dir = g_dir_open(POWER_SUPPLY, 0, nullptr);
            if (!dir)
                return "";
            std::string found;
            while (const char* entry = g_dir_read_name(dir)) {
                const std::string base = std::string(POWER_SUPPLY) + "/" + entry;
                char* type = nullptr;
                if (g_file_get_contents((base + "/type").c_str(), &type, nullptr, nullptr)) {
                    const bool mains = g_str_has_prefix(type, "Mains");
                    g_free(type);
                    if (mains) {
                        found = base + "/online";
                        break;
                    }
                }
            }
            g_dir_close(dir);
            return found;
        }

        GVariant* cached(GDBusProxy* proxy, const char* name, const GVariantType* type) {
            GVariant* v = g_dbus_proxy_get_cached_property(proxy, name);
            if (v && !g_variant_is_of_type(v, type)) {
                g_variant_unref(v);
                return nullptr;
            }
            return v;
        }

    } // namespace

    Charge charge_from_upower(guint32 state) {
        switch (state) {
        case 1: // charging
        case 5: // pending charge
            return Charge::Charging;
        case 2: // discharging
        case 3: // empty
        case 6: // pending discharge
            return Charge::Discharging;
        case 4:
            return Charge::Full;
        default:
            return Charge::Unknown;
        }
    }

    Charge charge_from(int ac, guint32 upower_state, double percent) {
        const Charge upower = charge_from_upower(upower_state);
        if (ac < 0)
            return upower;
        if (ac == 0)
            return Charge::Discharging;
        if (upower == Charge::Charging)
            return Charge::Charging;
        return upower == Charge::Full || percent >= 99 ? Charge::Full : Charge::Charging;
    }

    std::string format_duration(gint64 seconds) {
        if (seconds <= 0)
            return "";
        const gint64 minutes = (seconds + 30) / 60;
        if (minutes < 60)
            return std::to_string(minutes) + " min";
        const gint64 rest = minutes % 60;
        return std::to_string(minutes / 60) + " h" + (rest ? " " + std::to_string(rest) + " min" : "");
    }

    std::string battery_icon(double percent, Charge charge) {
        const int level = std::clamp(static_cast<int>((percent + 5) / 10), 0, 10) * 10;
        // Full is below 100 when a charge limit holds it on AC; MDI has no plugged-in glyph, so it shows as charging.
        if (charge == Charge::Charging || charge == Charge::Full)
            return level == 0 ? "fenriz-battery-charging-outline-symbolic"
                              : "fenriz-battery-charging-" + std::to_string(level) + "-symbolic";
        if (level == 0)
            return "fenriz-battery-outline-symbolic";
        if (level == 100)
            return "fenriz-battery-symbolic";
        return "fenriz-battery-" + std::to_string(level) + "-symbolic";
    }

    BatteryChange battery_change(const Battery& before, const Battery& now, bool first) {
        BatteryChange c;
        if (!now.present)
            return c;
        const bool low = now.charge == Charge::Discharging && now.percent <= LOW_BATTERY;
        if (first) {
            c.low = low;
            return c;
        }
        const bool was_charging = before.charge == Charge::Charging || before.charge == Charge::Full;
        c.plugged = now.charge == Charge::Charging && !was_charging;
        // Unknown -> Discharging is UPower settling after start, not an unplug
        c.unplugged = now.charge == Charge::Discharging && was_charging;
        const bool was_low = before.charge == Charge::Discharging && before.percent <= LOW_BATTERY;
        c.low = low && !was_low;
        c.recovered = was_low && !low;
        return c;
    }

    Power::~Power() {
        if (ac_tick_id_)
            g_source_remove(ac_tick_id_);
        if (cancel_) {
            g_cancellable_cancel(cancel_);
            g_object_unref(cancel_);
        }
        for (GDBusProxy* p : {battery_proxy_, profiles_proxy_})
            if (p) {
                g_signal_handlers_disconnect_by_data(p, this);
                g_object_unref(p);
            }
    }

    void Power::start() {
        cancel_ = g_cancellable_new();
        ac_online_path_ = find_ac_online_path();
        if (!ac_online_path_.empty()) {
            read_ac();
            ac_tick_id_ = g_timeout_add_seconds(AC_POLL_SECONDS, on_ac_tick, this);
        }
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 nullptr,
                                 "org.freedesktop.UPower",
                                 "/org/freedesktop/UPower/devices/DisplayDevice",
                                 "org.freedesktop.UPower.Device",
                                 cancel_,
                                 on_battery_proxy,
                                 this);
        connect_profiles(PROFILES_NAME, PROFILES_PATH);
    }

    void Power::connect_profiles(const char* name, const char* path) {
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                 nullptr,
                                 name,
                                 path,
                                 name,
                                 cancel_,
                                 on_profiles_proxy,
                                 this);
    }

    void Power::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void Power::notify() {
        for (auto& listener : listeners_)
            listener();
    }

    // Returns whether AC changed.
    bool Power::read_ac() {
        char* text = nullptr;
        int ac = -1;
        if (g_file_get_contents(ac_online_path_.c_str(), &text, nullptr, nullptr)) {
            ac = text[0] == '1' ? 1 : 0;
            g_free(text);
        }
        const bool changed = ac != ac_;
        ac_ = ac;
        return changed;
    }

    gboolean Power::on_ac_tick(gpointer data) {
        auto* self = static_cast<Power*>(data);
        if (self->read_ac() && self->battery_proxy_) {
            self->read_battery();
            self->notify();
        }
        return G_SOURCE_CONTINUE;
    }

    void Power::on_battery_proxy(GObject*, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GDBusProxy* proxy = g_dbus_proxy_new_for_bus_finish(res, &err);
        if (!proxy) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_message("power: no UPower: %s", err->message);
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Power*>(data);
        self->battery_proxy_ = proxy;
        g_signal_connect_swapped(proxy,
                                 "g-properties-changed",
                                 G_CALLBACK(+[](Power* power) {
                                     power->read_battery();
                                     power->notify();
                                 }),
                                 self);
        self->read_battery();
        self->notify();
    }

    void Power::read_battery() {
        Battery b;
        if (GVariant* v = cached(battery_proxy_, "IsPresent", G_VARIANT_TYPE_BOOLEAN)) {
            b.present = g_variant_get_boolean(v);
            g_variant_unref(v);
        }
        if (GVariant* v = cached(battery_proxy_, "Percentage", G_VARIANT_TYPE_DOUBLE)) {
            b.percent = g_variant_get_double(v);
            g_variant_unref(v);
        }
        guint32 state = 0;
        if (GVariant* v = cached(battery_proxy_, "State", G_VARIANT_TYPE_UINT32)) {
            state = g_variant_get_uint32(v);
            g_variant_unref(v);
        }
        b.charge = charge_from(ac_, state, b.percent);
        // UPower's estimate follows its own state, so it only counts when that agrees
        const char* left = nullptr;
        if (b.charge == Charge::Charging && charge_from_upower(state) == Charge::Charging)
            left = "TimeToFull";
        else if (b.charge == Charge::Discharging && charge_from_upower(state) == Charge::Discharging)
            left = "TimeToEmpty";
        if (GVariant* v = left ? cached(battery_proxy_, left, G_VARIANT_TYPE_INT64) : nullptr) {
            b.seconds_left = g_variant_get_int64(v);
            g_variant_unref(v);
        }
        b.icon = battery_icon(b.percent, b.charge);
        battery_ = b;
    }

    void Power::on_profiles_proxy(GObject*, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GDBusProxy* proxy = g_dbus_proxy_new_for_bus_finish(res, &err);
        if (!proxy) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_message("power: %s", err->message);
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Power*>(data);
        char* owner = g_dbus_proxy_get_name_owner(proxy);
        if (!owner) {
            g_object_unref(proxy);
            if (!self->legacy_profiles_) {
                self->legacy_profiles_ = true;
                self->connect_profiles(LEGACY_PROFILES_NAME, LEGACY_PROFILES_PATH);
            } else {
                g_message("power: no power-profiles-daemon, so no power modes");
            }
            return;
        }
        g_free(owner);
        self->profiles_proxy_ = proxy;
        g_signal_connect_swapped(proxy,
                                 "g-properties-changed",
                                 G_CALLBACK(+[](Power* power) {
                                     power->read_profiles();
                                     power->notify();
                                 }),
                                 self);
        self->read_profiles();
        self->notify();
    }

    void Power::read_profiles() {
        profiles_.clear();
        if (GVariant* v = cached(profiles_proxy_, "Profiles", G_VARIANT_TYPE("aa{sv}"))) {
            GVariantIter it;
            g_variant_iter_init(&it, v);
            while (GVariant* dict = g_variant_iter_next_value(&it)) {
                const char* name = nullptr;
                if (g_variant_lookup(dict, "Profile", "&s", &name))
                    profiles_.push_back(name);
                g_variant_unref(dict);
            }
            g_variant_unref(v);
        }
        if (GVariant* v = cached(profiles_proxy_, "ActiveProfile", G_VARIANT_TYPE_STRING)) {
            profile_ = g_variant_get_string(v, nullptr);
            g_variant_unref(v);
        }
    }

    void Power::set_profile(const std::string& profile) {
        if (!profiles_proxy_ || profile == profile_)
            return;
        g_dbus_proxy_call(
            profiles_proxy_,
            "org.freedesktop.DBus.Properties.Set",
            g_variant_new("(ssv)",
                          g_dbus_proxy_get_interface_name(profiles_proxy_),
                          "ActiveProfile",
                          g_variant_new_string(profile.c_str())),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            [](GObject* source, GAsyncResult* res, gpointer) {
                GError* err = nullptr;
                if (GVariant* r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &err))
                    g_variant_unref(r);
                if (err) {
                    g_warning("power: could not set the power mode: %s", err->message);
                    g_error_free(err);
                }
            },
            nullptr);
    }

} // namespace fenriz::bar
