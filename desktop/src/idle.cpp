#include "idle.hpp"

#include <gdk/wayland/gdkwayland.h>
#include <glib.h>

#include "ext-idle-notify-v1-client-protocol.h"

namespace fenriz::desktop {

    struct IdleWatch {
        Idle::Handler idled;
        Idle::Handler resumed;
        ext_idle_notification_v1* notification = nullptr;
        int seconds = 0;
        bool is_idle = false;
    };

    namespace {

        struct Bind {
            ext_idle_notifier_v1* notifier = nullptr;
        };

        void on_global(void* data, wl_registry* registry, uint32_t name, const char* iface, uint32_t version) {
            auto* bind = static_cast<Bind*>(data);
            if (g_strcmp0(iface, ext_idle_notifier_v1_interface.name) == 0)
                bind->notifier = static_cast<ext_idle_notifier_v1*>(
                    wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, version < 1 ? version : 1));
        }

        void on_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener REGISTRY_LISTENER = {on_global, on_global_remove};

        void on_idled(void* data, ext_idle_notification_v1*) {
            auto* watch = static_cast<IdleWatch*>(data);
            watch->is_idle = true;
            if (watch->idled)
                watch->idled();
        }

        void on_resumed(void* data, ext_idle_notification_v1*) {
            auto* watch = static_cast<IdleWatch*>(data);
            watch->is_idle = false;
            if (watch->resumed)
                watch->resumed();
        }

        const ext_idle_notification_v1_listener NOTIFICATION_LISTENER = {on_idled, on_resumed};

    } // namespace

    Idle::Idle() = default;

    Idle::~Idle() {
        for (auto& watch : watches_)
            if (watch->notification)
                ext_idle_notification_v1_destroy(watch->notification);
        if (notifier_)
            ext_idle_notifier_v1_destroy(notifier_);
    }

    bool Idle::start() {
        GdkDisplay* display = gdk_display_get_default();
        if (!GDK_IS_WAYLAND_DISPLAY(display))
            return false;
        wl_display* wl = gdk_wayland_display_get_wl_display(display);
        GdkSeat* seat = gdk_display_get_default_seat(display);
        if (!wl || !seat)
            return false;

        Bind bind;
        wl_registry* registry = wl_display_get_registry(wl);
        wl_registry_add_listener(registry, &REGISTRY_LISTENER, &bind);
        wl_display_roundtrip(wl);
        wl_registry_destroy(registry);

        if (!bind.notifier) {
            g_warning("idle: compositor does not support ext-idle-notify-v1; idle actions are off");
            return false;
        }
        notifier_ = bind.notifier;
        seat_ = gdk_wayland_seat_get_wl_seat(seat);
        return true;
    }

    void Idle::arm(IdleWatch& watch) {
        if (watch.notification)
            return;
        watch.notification =
            ext_idle_notifier_v1_get_idle_notification(notifier_, static_cast<uint32_t>(watch.seconds) * 1000, seat_);
        ext_idle_notification_v1_add_listener(watch.notification, &NOTIFICATION_LISTENER, &watch);
        wl_display_flush(gdk_wayland_display_get_wl_display(gdk_display_get_default()));
    }

    void Idle::watch(int seconds, Handler idled, Handler resumed) {
        if (!notifier_ || seconds <= 0)
            return;

        auto watch = std::make_unique<IdleWatch>();
        watch->idled = std::move(idled);
        watch->resumed = std::move(resumed);
        watch->seconds = seconds;
        if (!inhibited_)
            arm(*watch);
        watches_.push_back(std::move(watch));
    }

    void Idle::set_inhibited(bool inhibited) {
        if (!notifier_ || inhibited == inhibited_)
            return;
        inhibited_ = inhibited;

        for (auto& watch : watches_) {
            if (!inhibited) {
                arm(*watch);
                continue;
            }
            if (watch->is_idle) {
                watch->is_idle = false;
                if (watch->resumed)
                    watch->resumed();
            }
            if (watch->notification) {
                ext_idle_notification_v1_destroy(watch->notification);
                watch->notification = nullptr;
            }
        }
        wl_display_flush(gdk_wayland_display_get_wl_display(gdk_display_get_default()));
    }

} // namespace fenriz::desktop
