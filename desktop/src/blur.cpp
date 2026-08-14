#include "blur.hpp"

#include <gdk/wayland/gdkwayland.h>

#include <algorithm>
#include <cmath>

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
            GtkWidget* widget; // the card, or its container in Mode::Children
            Mode mode;
            int radius;
            ext_background_effect_surface_v1* effect = nullptr;
            GdkSurface* surface = nullptr; // the one `effect` was made for
            guint idle = 0;
        };

        void add_rounded(wl_region* region, int x, int y, int w, int h, int r) {
            Band bands[MAX_BANDS];
            const int n = rounded_bands(w, h, r, bands);
            for (int i = 0; i < n; i++)
                wl_region_add(region, x + bands[i].x, y + bands[i].y, bands[i].w, bands[i].h);
        }

        void add_widget(wl_region* region, const Target& target, GtkWidget* widget) {
            graphene_rect_t bounds;
            if (!gtk_widget_compute_bounds(widget, GTK_WIDGET(target.native), &bounds))
                return;
            const int x = (int)bounds.origin.x, y = (int)bounds.origin.y;
            const int w = (int)bounds.size.width, h = (int)bounds.size.height;
            if (w <= 0 || h <= 0)
                return;
            add_rounded(region, x, y, w, h, target.radius);
        }

        gboolean apply(gpointer data) {
            auto* target = static_cast<Target*>(data);
            target->idle = 0;
            if (!target->effect)
                return G_SOURCE_REMOVE;

            GdkDisplay* display = gdk_display_get_default();
            wl_compositor* compositor = gdk_wayland_display_get_wl_compositor(GDK_WAYLAND_DISPLAY(display));
            wl_region* region = wl_compositor_create_region(compositor);

            switch (target->mode) {
            case Mode::Widget:
                add_widget(region, *target, target->widget);
                break;
            case Mode::Children:
                for (GtkWidget* child = gtk_widget_get_first_child(target->widget); child;
                     child = gtk_widget_get_next_sibling(child))
                    if (gtk_widget_get_mapped(child))
                        add_widget(region, *target, child);
                break;
            }

            ext_background_effect_surface_v1_set_blur_region(target->effect, region);
            wl_region_destroy(region);
            gtk_widget_queue_draw(GTK_WIDGET(target->native));
            return G_SOURCE_REMOVE;
        }

        void schedule(Target* target) {
            if (!target->idle && target->effect)
                target->idle = g_idle_add_full(G_PRIORITY_LOW, apply, target, nullptr);
        }

        void on_surface_changed(GObject*, GParamSpec*, gpointer data) { schedule(static_cast<Target*>(data)); }

        void detach(Target* target) {
            if (target->idle) {
                g_source_remove(target->idle);
                target->idle = 0;
            }
            if (target->effect) {
                ext_background_effect_surface_v1_destroy(target->effect);
                target->effect = nullptr;
            }
            if (target->surface) {
                g_signal_handlers_disconnect_by_func(target->surface, (gpointer)on_surface_changed, target);
                target->surface = nullptr;
            }
        }

        void on_realize(GtkWidget*, gpointer data) {
            auto* target = static_cast<Target*>(data);
            detach(target);

            GdkSurface* surface = gtk_native_get_surface(target->native);
            if (!GDK_IS_WAYLAND_SURFACE(surface))
                return;
            wl_surface* wl = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
            if (!wl)
                return;

            target->surface = surface;
            target->effect = ext_background_effect_manager_v1_get_background_effect(global.manager, wl);
            g_signal_connect(surface, "notify::width", G_CALLBACK(on_surface_changed), target);
            g_signal_connect(surface, "notify::height", G_CALLBACK(on_surface_changed), target);
            g_signal_connect(surface, "notify::mapped", G_CALLBACK(on_surface_changed), target);
            schedule(target);
        }

        void on_unrealize(GtkWidget*, gpointer data) { detach(static_cast<Target*>(data)); }

        void free_target(gpointer data) {
            auto* target = static_cast<Target*>(data);
            detach(target);
            delete target;
        }

    } // namespace

    int rounded_bands(int w, int h, int r, Band out[MAX_BANDS]) {
        if (w <= 0 || h <= 0)
            return 0;
        r = std::min(r, std::min(w, h) / 2);
        if (r <= 0) {
            out[0] = {0, 0, w, h};
            return 1;
        }

        const int steps = 2 * r >= h || 2 * r >= w ? 2 : 1;

        int n = 0;
        if (h > 2 * r)
            out[n++] = {0, r, w, h - 2 * r};

        for (int i = 0; i < steps; i++) {
            const int y0 = r * i / steps, y1 = r * (i + 1) / steps;
            const double d = r - y0;
            const int in = (int)std::ceil(r - std::sqrt((double)r * r - d * d));
            if (y1 <= y0 || w <= 2 * in)
                continue;
            out[n++] = {in, y0, w - 2 * in, y1 - y0};
            out[n++] = {in, h - y1, w - 2 * in, y1 - y0};
        }
        return n;
    }

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

    void attach(GtkNative* native, GtkWidget* widget, Mode mode, int radius) {
        if (!global.manager || !widget)
            return;

        auto* target = new Target{native, widget, mode, radius};
        GtkWidget* w = GTK_WIDGET(native);
        g_object_set_data_full(G_OBJECT(w), "fenriz-blur", target, free_target);
        g_signal_connect(w, "realize", G_CALLBACK(on_realize), target);
        g_signal_connect(w, "unrealize", G_CALLBACK(on_unrealize), target);
        if (gtk_widget_get_realized(w))
            on_realize(w, target);
    }

} // namespace fenriz::desktop::blur
