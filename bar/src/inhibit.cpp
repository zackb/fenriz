#include "inhibit.hpp"

#include <gdk/wayland/gdkwayland.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

namespace fenriz::bar {

    namespace {

        zwp_idle_inhibit_manager_v1* manager = nullptr;
        bool bound = false;

        void on_global(void*, wl_registry* registry, uint32_t name, const char* iface, uint32_t) {
            if (g_strcmp0(iface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
                manager = static_cast<zwp_idle_inhibit_manager_v1*>(
                    wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1));
        }

        void on_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener REGISTRY_LISTENER = {on_global, on_global_remove};

        // Bound once, on first use.
        zwp_idle_inhibit_manager_v1* get_manager() {
            if (bound)
                return manager;
            bound = true;
            GdkDisplay* display = gdk_display_get_default();
            if (!GDK_IS_WAYLAND_DISPLAY(display))
                return nullptr;
            wl_display* wl = gdk_wayland_display_get_wl_display(display);
            wl_registry* registry = wl_display_get_registry(wl);
            wl_registry_add_listener(registry, &REGISTRY_LISTENER, nullptr);
            wl_display_roundtrip(wl);
            wl_registry_destroy(registry);
            if (!manager)
                g_message("inhibit: compositor does not support idle-inhibit-unstable-v1");
            return manager;
        }

    } // namespace

    IdleInhibitor::IdleInhibitor(GtkWindow* window) : window_(window) {
        map_handler_ =
            g_signal_connect_swapped(window, "map", G_CALLBACK(+[](IdleInhibitor* self) { self->apply(); }), this);
    }

    IdleInhibitor::~IdleInhibitor() {
        if (map_handler_)
            g_signal_handler_disconnect(window_, map_handler_);
        if (inhibitor_)
            zwp_idle_inhibitor_v1_destroy(inhibitor_);
    }

    bool IdleInhibitor::available() const { return get_manager() != nullptr; }

    void IdleInhibitor::set(bool on) {
        wanted_ = on;
        apply();
    }

    void IdleInhibitor::apply() {
        if (inhibitor_) {
            zwp_idle_inhibitor_v1_destroy(inhibitor_);
            inhibitor_ = nullptr;
        }
        if (!wanted_ || !get_manager())
            return;
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
        if (!surface || !GDK_IS_WAYLAND_SURFACE(surface))
            return; // not realized yet; the map handler comes back
        wl_surface* wl = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
        if (!wl)
            return;
        inhibitor_ = zwp_idle_inhibit_manager_v1_create_inhibitor(get_manager(), wl);
        g_message("inhibit: keeping the session awake");
    }

} // namespace fenriz::bar
