#include "idle.hpp"

#include <gdk/wayland/gdkwayland.h>
#include <glib.h>

#include "ext-idle-notify-v1-client-protocol.h"

namespace fenriz::desktop {

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
            auto* handler = static_cast<Idle::Handler*>(data);
            if (*handler)
                (*handler)();
        }

        void on_resumed(void*, ext_idle_notification_v1*) {}

        const ext_idle_notification_v1_listener NOTIFICATION_LISTENER = {on_idled, on_resumed};

    } // namespace

    Idle::Idle(const Config& cfg) : cfg_(cfg) {}

    Idle::~Idle() {
        if (notification_)
            ext_idle_notification_v1_destroy(notification_);
        if (notifier_)
            ext_idle_notifier_v1_destroy(notifier_);
    }

    void Idle::start(Handler on_idle) {
        if (cfg_.idle_lock <= 0)
            return;

        GdkDisplay* display = gdk_display_get_default();
        if (!GDK_IS_WAYLAND_DISPLAY(display))
            return;
        wl_display* wl = gdk_wayland_display_get_wl_display(display);
        GdkSeat* seat = gdk_display_get_default_seat(display);
        if (!wl || !seat)
            return;

        Bind bind;
        wl_registry* registry = wl_display_get_registry(wl);
        wl_registry_add_listener(registry, &REGISTRY_LISTENER, &bind);
        wl_display_roundtrip(wl);
        wl_registry_destroy(registry);

        if (!bind.notifier) {
            g_warning("idle: compositor does not support ext-idle-notify-v1; idle lock is off");
            return;
        }
        notifier_ = bind.notifier;
        on_idle_ = std::move(on_idle);

        notification_ = ext_idle_notifier_v1_get_idle_notification(
            notifier_, static_cast<uint32_t>(cfg_.idle_lock) * 1000, gdk_wayland_seat_get_wl_seat(seat));
        ext_idle_notification_v1_add_listener(notification_, &NOTIFICATION_LISTENER, &on_idle_);
        wl_display_flush(wl);

        g_message("idle: locking after %d seconds", cfg_.idle_lock);
    }

} // namespace fenriz::desktop
