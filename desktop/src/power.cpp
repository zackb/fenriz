#include "power.hpp"

#include <gdk/wayland/gdkwayland.h>
#include <glib.h>

#include "wlr-output-power-management-unstable-v1-client-protocol.h"

namespace fenriz::desktop {

    // One screen's power control.
    struct OutputPowerControl {
        wl_output* output = nullptr;
        zwlr_output_power_v1* power = nullptr;
    };

    namespace {

        struct Bind {
            zwlr_output_power_manager_v1* manager = nullptr;
        };

        void on_global(void* data, wl_registry* registry, uint32_t name, const char* iface, uint32_t version) {
            auto* bind = static_cast<Bind*>(data);
            if (g_strcmp0(iface, zwlr_output_power_manager_v1_interface.name) == 0)
                bind->manager = static_cast<zwlr_output_power_manager_v1*>(
                    wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1));
        }

        void on_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener REGISTRY_LISTENER = {on_global, on_global_remove};

        void on_mode(void*, zwlr_output_power_v1*, uint32_t) {}

        void on_failed(void* data, zwlr_output_power_v1*) {
            auto* control = static_cast<OutputPowerControl*>(data);
            if (control->power) {
                zwlr_output_power_v1_destroy(control->power);
                control->power = nullptr;
            }
        }

        const zwlr_output_power_v1_listener POWER_LISTENER = {on_mode, on_failed};

        wl_display* display() {
            GdkDisplay* gdk = gdk_display_get_default();
            return GDK_IS_WAYLAND_DISPLAY(gdk) ? gdk_wayland_display_get_wl_display(gdk) : nullptr;
        }

    } // namespace

    OutputPower::OutputPower() = default;

    OutputPower::~OutputPower() {
        for (auto& control : controls_)
            if (control->power)
                zwlr_output_power_v1_destroy(control->power);
        if (manager_)
            zwlr_output_power_manager_v1_destroy(manager_);
    }

    bool OutputPower::start() {
        wl_display* wl = display();
        if (!wl)
            return false;

        Bind bind;
        wl_registry* registry = wl_display_get_registry(wl);
        wl_registry_add_listener(registry, &REGISTRY_LISTENER, &bind);
        wl_display_roundtrip(wl);
        wl_registry_destroy(registry);

        if (!bind.manager) {
            g_warning("power: compositor does not support wlr-output-power-management; blanking is off");
            return false;
        }
        manager_ = bind.manager;
        return true;
    }

    void OutputPower::set_all(bool on) {
        wl_display* wl = display();
        if (!manager_ || !wl)
            return;

        // reconcile against the monitors that exist right now
        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        std::vector<std::unique_ptr<OutputPowerControl>> current;

        for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
            auto* monitor = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i));
            wl_output* output = GDK_IS_WAYLAND_MONITOR(monitor) ? gdk_wayland_monitor_get_wl_output(monitor) : nullptr;
            g_object_unref(monitor);
            if (!output)
                continue;

            std::unique_ptr<OutputPowerControl> control;
            for (auto& existing : controls_)
                if (existing && existing->output == output && existing->power)
                    control = std::move(existing);

            if (!control) {
                control = std::make_unique<OutputPowerControl>();
                control->output = output;
                control->power = zwlr_output_power_manager_v1_get_output_power(manager_, output);
                zwlr_output_power_v1_add_listener(control->power, &POWER_LISTENER, control.get());
            }
            zwlr_output_power_v1_set_mode(control->power,
                                          on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
            current.push_back(std::move(control));
        }

        // whatever is left belonged to a monitor that is gone.
        for (auto& stale : controls_)
            if (stale && stale->power)
                zwlr_output_power_v1_destroy(stale->power);
        controls_ = std::move(current);

        wl_display_flush(wl);
    }

} // namespace fenriz::desktop
