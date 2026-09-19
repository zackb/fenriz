#include "clipboard.hpp"

#include <gdk/wayland/gdkwayland.h>
#include <gio/gunixoutputstream.h>
#include <unistd.h>

#include "ext-data-control-v1-client-protocol.h"

namespace fenriz::bar {

    namespace {

        constexpr const char* MIME = "image/png";

        ext_data_control_manager_v1* manager = nullptr;
        ext_data_control_device_v1* device = nullptr;
        bool bound = false;

        ext_data_control_source_v1* source = nullptr;
        GBytes* content = nullptr;

        void on_global(void*, wl_registry* registry, uint32_t name, const char* iface, uint32_t) {
            if (g_strcmp0(iface, ext_data_control_manager_v1_interface.name) == 0)
                manager = static_cast<ext_data_control_manager_v1*>(
                    wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
        }

        void on_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener REGISTRY_LISTENER = {on_global, on_global_remove};

        void on_data_offer(void*, ext_data_control_device_v1*, ext_data_control_offer_v1*) {}

        void on_selection(void*, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
            if (offer)
                ext_data_control_offer_v1_destroy(offer);
        }

        void on_finished(void*, ext_data_control_device_v1* d) {
            ext_data_control_device_v1_destroy(d);
            device = nullptr;
        }

        const ext_data_control_device_v1_listener DEVICE_LISTENER = {
            on_data_offer, on_selection, on_finished, on_selection};

        bool bind() {
            if (bound)
                return device != nullptr;
            bound = true;
            GdkDisplay* display = gdk_display_get_default();
            if (!GDK_IS_WAYLAND_DISPLAY(display))
                return false;
            wl_display* wl = gdk_wayland_display_get_wl_display(display);
            wl_registry* registry = wl_display_get_registry(wl);
            wl_registry_add_listener(registry, &REGISTRY_LISTENER, nullptr);
            wl_display_roundtrip(wl);
            wl_registry_destroy(registry);
            GdkSeat* seat = gdk_display_get_default_seat(display);
            if (!manager || !seat) {
                g_message("clipboard: compositor does not support ext-data-control-v1");
                return false;
            }
            device = ext_data_control_manager_v1_get_data_device(manager, gdk_wayland_seat_get_wl_seat(seat));
            ext_data_control_device_v1_add_listener(device, &DEVICE_LISTENER, nullptr);
            return true;
        }

        void on_written(GObject* stream, GAsyncResult* res, gpointer data) {
            GError* err = nullptr;
            if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), res, nullptr, &err)) {
                g_message("clipboard: %s", err->message); // the reader went away early
                g_error_free(err);
            }
            g_bytes_unref(static_cast<GBytes*>(data));
            g_object_unref(stream); // closes the fd
        }

        void on_send(void*, ext_data_control_source_v1*, const char* mime, int32_t fd) {
            if (g_strcmp0(mime, MIME) != 0 || !content) {
                close(fd);
                return;
            }
            // a reader that is slow to drain must not stall the bar
            GOutputStream* out = g_unix_output_stream_new(fd, TRUE);
            GBytes* bytes = g_bytes_ref(content);
            gsize size = 0;
            const void* data = g_bytes_get_data(bytes, &size);
            g_output_stream_write_all_async(out, data, size, G_PRIORITY_DEFAULT, nullptr, on_written, bytes);
        }

        void drop_source() {
            if (source)
                ext_data_control_source_v1_destroy(source);
            source = nullptr;
            g_clear_pointer(&content, g_bytes_unref);
        }

        void on_cancelled(void*, ext_data_control_source_v1*) { drop_source(); }

        const ext_data_control_source_v1_listener SOURCE_LISTENER = {on_send, on_cancelled};

    } // namespace

    bool copy_png(GBytes* png) {
        if (!bind())
            return false;
        drop_source();
        content = g_bytes_ref(png);
        source = ext_data_control_manager_v1_create_data_source(manager);
        ext_data_control_source_v1_add_listener(source, &SOURCE_LISTENER, nullptr);
        ext_data_control_source_v1_offer(source, MIME);
        ext_data_control_device_v1_set_selection(device, source);
        return true;
    }

} // namespace fenriz::bar
