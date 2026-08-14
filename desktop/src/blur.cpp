#include "blur.hpp"

#include <gdk/wayland/gdkwayland.h>

#include "ext-background-effect-v1-client-protocol.h"

namespace fenriz::desktop::blur {

    namespace {

        struct Bind {
            ext_background_effect_manager_v1* manager = nullptr;
            uint32_t capabilities = 0;
        };

        Bind global;

        void on_capabilities(void* data, ext_background_effect_manager_v1*, uint32_t caps) {
            static_cast<Bind*>(data)->capabilities = caps;
        }

        const ext_background_effect_manager_v1_listener MANAGER_LISTENER = {on_capabilities};

        void on_global(void* data, wl_registry* registry, uint32_t name, const char* iface, uint32_t) {
            auto* bind = static_cast<Bind*>(data);
            if (g_strcmp0(iface, ext_background_effect_manager_v1_interface.name) == 0)
                bind->manager = static_cast<ext_background_effect_manager_v1*>(
                    wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
        }

        void on_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener REGISTRY_LISTENER = {on_global, on_global_remove};

        struct Target {
            GtkNative* native;
            ext_background_effect_surface_v1* effect = nullptr;
        };

        constexpr int WHOLE_SURFACE = 1 << 20;

        void on_realize(GtkWidget*, gpointer data) {
            auto* target = static_cast<Target*>(data);
            if (target->effect)
                return;

            GdkSurface* surface = gtk_native_get_surface(target->native);
            if (!GDK_IS_WAYLAND_SURFACE(surface))
                return;
            wl_surface* wl = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
            if (!wl)
                return;
            target->effect = ext_background_effect_manager_v1_get_background_effect(global.manager, wl);

            GdkDisplay* display = gdk_display_get_default();
            wl_compositor* compositor = gdk_wayland_display_get_wl_compositor(GDK_WAYLAND_DISPLAY(display));
            wl_region* region = wl_compositor_create_region(compositor);
            wl_region_add(region, 0, 0, WHOLE_SURFACE, WHOLE_SURFACE);
            ext_background_effect_surface_v1_set_blur_region(target->effect, region);
            wl_region_destroy(region);
            gtk_widget_queue_draw(GTK_WIDGET(target->native));
        }

        // The wl_surface goes with the GdkSurface, and the effect object with it.
        void on_unrealize(GtkWidget*, gpointer data) {
            auto* target = static_cast<Target*>(data);
            if (target->effect) {
                ext_background_effect_surface_v1_destroy(target->effect);
                target->effect = nullptr;
            }
        }

        void free_target(gpointer data) {
            auto* target = static_cast<Target*>(data);
            on_unrealize(nullptr, target);
            delete target;
        }

    } // namespace

    bool init() {
        GdkDisplay* display = gdk_display_get_default();
        if (!GDK_IS_WAYLAND_DISPLAY(display))
            return false;
        wl_display* wl = gdk_wayland_display_get_wl_display(display);
        if (!wl)
            return false;

        wl_registry* registry = wl_display_get_registry(wl);
        wl_registry_add_listener(registry, &REGISTRY_LISTENER, &global);
        wl_display_roundtrip(wl); // the manager
        if (global.manager) {
            ext_background_effect_manager_v1_add_listener(global.manager, &MANAGER_LISTENER, &global);
            wl_display_roundtrip(wl); // its capabilities
        }
        wl_registry_destroy(registry);

        if (!global.manager) {
            g_message("blur: compositor does not support ext-background-effect-v1");
            return false;
        }
        if (!(global.capabilities & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR)) {
            g_message("blur: compositor has blur disabled");
            ext_background_effect_manager_v1_destroy(global.manager);
            global.manager = nullptr;
            return false;
        }
        return true;
    }

    void attach(GtkNative* native) {
        if (!global.manager)
            return;

        auto* target = new Target{native};
        GtkWidget* w = GTK_WIDGET(native);
        g_object_set_data_full(G_OBJECT(w), "fenriz-blur", target, free_target);
        g_signal_connect(w, "realize", G_CALLBACK(on_realize), target);
        g_signal_connect(w, "unrealize", G_CALLBACK(on_unrealize), target);
        if (gtk_widget_get_realized(w))
            on_realize(w, target);
    }

} // namespace fenriz::desktop::blur
