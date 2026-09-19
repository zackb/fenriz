#include "capture.hpp"

#include <gdk/wayland/gdkwayland.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iterator>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

namespace fenriz::bar {

    namespace {

        ext_image_copy_capture_manager_v1* copy_manager = nullptr;
        ext_output_image_capture_source_manager_v1* source_manager = nullptr;
        wl_shm* shm = nullptr;
        bool bound = false;

        void on_global(void*, wl_registry* registry, uint32_t name, const char* iface, uint32_t) {
            if (g_strcmp0(iface, ext_image_copy_capture_manager_v1_interface.name) == 0)
                copy_manager = static_cast<ext_image_copy_capture_manager_v1*>(
                    wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
            else if (g_strcmp0(iface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
                source_manager = static_cast<ext_output_image_capture_source_manager_v1*>(
                    wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
            else if (g_strcmp0(iface, wl_shm_interface.name) == 0)
                shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        }

        void on_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener REGISTRY_LISTENER = {on_global, on_global_remove};

        // Bound once, on first use.
        bool bind() {
            if (bound)
                return copy_manager && source_manager && shm;
            bound = true;
            GdkDisplay* display = gdk_display_get_default();
            if (!GDK_IS_WAYLAND_DISPLAY(display))
                return false;
            wl_display* wl = gdk_wayland_display_get_wl_display(display);
            wl_registry* registry = wl_display_get_registry(wl);
            wl_registry_add_listener(registry, &REGISTRY_LISTENER, nullptr);
            wl_display_roundtrip(wl);
            wl_registry_destroy(registry);
            if (!copy_manager || !source_manager || !shm)
                g_message("capture: compositor does not support ext-image-copy-capture-v1 output capture");
            return copy_manager && source_manager && shm;
        }

        // One capture in flight: source, session, then a single frame into an shm buffer.
        struct Job {
            CaptureDone done;
            ext_image_capture_source_v1* source = nullptr;
            ext_image_copy_capture_session_v1* session = nullptr;
            ext_image_copy_capture_frame_v1* frame = nullptr;
            wl_buffer* buffer = nullptr;
            void* data = MAP_FAILED;
            size_t size = 0;
            int width = 0, height = 0;
            int format = -1; // chosen wl_shm format
            uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;

            ~Job() {
                if (buffer)
                    wl_buffer_destroy(buffer);
                if (data != MAP_FAILED)
                    munmap(data, size);
                if (frame)
                    ext_image_copy_capture_frame_v1_destroy(frame);
                if (session)
                    ext_image_copy_capture_session_v1_destroy(session);
                if (source)
                    ext_image_capture_source_v1_destroy(source);
            }

            void finish(cairo_surface_t* image) {
                CaptureDone cb = std::move(done);
                delete this;
                cb(image);
            }
        };

        // The 8-bit formats we read, most preferred first.
        constexpr uint32_t FORMATS[] = {
            WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XRGB8888, WL_SHM_FORMAT_ABGR8888, WL_SHM_FORMAT_XBGR8888};

        int preference(uint32_t format) {
            for (size_t i = 0; i < std::size(FORMATS); i++)
                if (FORMATS[i] == format)
                    return static_cast<int>(i);
            return -1;
        }

        cairo_surface_t* to_surface(const Job& job) {
            const uint32_t format = static_cast<uint32_t>(job.format);
            const bool alpha = format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_ABGR8888;
            const bool swap = format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888;
            cairo_surface_t* out =
                cairo_image_surface_create(alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, job.width, job.height);
            const int dst_stride = cairo_image_surface_get_stride(out);
            const auto* src = static_cast<const unsigned char*>(job.data);
            unsigned char* dst = cairo_image_surface_get_data(out);
            for (int y = 0; y < job.height; y++) {
                auto* row = dst + y * dst_stride;
                std::memcpy(row, src + y * job.width * 4, job.width * 4);
                if (swap)
                    for (int x = 0; x < job.width; x++) {
                        auto* px = reinterpret_cast<uint32_t*>(row) + x;
                        *px = (*px & 0xff00ff00) | ((*px & 0xff) << 16) | ((*px >> 16) & 0xff);
                    }
            }
            cairo_surface_mark_dirty(out);
            return out;
        }

        void on_transform(void* data, ext_image_copy_capture_frame_v1*, uint32_t transform) {
            static_cast<Job*>(data)->transform = transform;
        }
        void on_damage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
        void on_presentation_time(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}

        void on_ready(void* data, ext_image_copy_capture_frame_v1*) {
            auto* job = static_cast<Job*>(data);
            if (job->transform != WL_OUTPUT_TRANSFORM_NORMAL)
                g_warning("capture: output transform %u is not applied", job->transform);
            job->finish(to_surface(*job));
        }

        void on_failed(void* data, ext_image_copy_capture_frame_v1*, uint32_t reason) {
            g_warning("capture: frame failed (reason %u)", reason);
            static_cast<Job*>(data)->finish(nullptr);
        }

        const ext_image_copy_capture_frame_v1_listener FRAME_LISTENER = {
            on_transform, on_damage, on_presentation_time, on_ready, on_failed};

        void on_buffer_size(void* data, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
            auto* job = static_cast<Job*>(data);
            job->width = static_cast<int>(w);
            job->height = static_cast<int>(h);
        }

        void on_shm_format(void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
            auto* job = static_cast<Job*>(data);
            const int p = preference(format);
            if (p >= 0 && (job->format < 0 || p < preference(static_cast<uint32_t>(job->format))))
                job->format = static_cast<int>(format);
        }

        void on_dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
        void on_dmabuf_format(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}

        bool allocate(Job& job) {
            const int stride = job.width * 4;
            job.size = static_cast<size_t>(stride) * job.height;
            const int fd = memfd_create("fenriz-shot", MFD_CLOEXEC);
            if (fd < 0)
                return false;
            if (ftruncate(fd, static_cast<off_t>(job.size)) < 0) {
                close(fd);
                return false;
            }
            job.data = mmap(nullptr, job.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (job.data == MAP_FAILED) {
                close(fd);
                return false;
            }
            wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(job.size));
            job.buffer = wl_shm_pool_create_buffer(pool, 0, job.width, job.height, stride, job.format);
            wl_shm_pool_destroy(pool);
            close(fd);
            return true;
        }

        void on_done(void* data, ext_image_copy_capture_session_v1*) {
            auto* job = static_cast<Job*>(data);
            if (job->frame)
                return;
            if (job->format < 0 || job->width <= 0 || job->height <= 0) {
                g_warning("capture: no readable shm format offered");
                job->finish(nullptr);
                return;
            }
            if (!allocate(*job)) {
                g_warning("capture: shm buffer: %s", g_strerror(errno));
                job->finish(nullptr);
                return;
            }
            job->frame = ext_image_copy_capture_session_v1_create_frame(job->session);
            ext_image_copy_capture_frame_v1_add_listener(job->frame, &FRAME_LISTENER, job);
            ext_image_copy_capture_frame_v1_attach_buffer(job->frame, job->buffer);
            ext_image_copy_capture_frame_v1_damage_buffer(job->frame, 0, 0, job->width, job->height);
            ext_image_copy_capture_frame_v1_capture(job->frame);
        }

        void on_stopped(void* data, ext_image_copy_capture_session_v1*) {
            auto* job = static_cast<Job*>(data);
            if (job->frame)
                return; // the frame reports failed on its own
            g_warning("capture: session stopped");
            job->finish(nullptr);
        }

        const ext_image_copy_capture_session_v1_listener SESSION_LISTENER = {
            on_buffer_size, on_shm_format, on_dmabuf_device, on_dmabuf_format, on_done, on_stopped};

    } // namespace

    bool capture_available() { return bind(); }

    void capture_output(GdkMonitor* monitor, CaptureDone done) {
        wl_output* output = GDK_IS_WAYLAND_MONITOR(monitor) ? gdk_wayland_monitor_get_wl_output(monitor) : nullptr;
        if (!bind() || !output) {
            // always asynchronous, so a caller looping over outputs never sees its callback mid-loop
            g_idle_add_once(
                [](gpointer data) {
                    auto* cb = static_cast<CaptureDone*>(data);
                    (*cb)(nullptr);
                    delete cb;
                },
                new CaptureDone(std::move(done)));
            return;
        }
        auto* job = new Job{std::move(done)};
        job->source = ext_output_image_capture_source_manager_v1_create_source(source_manager, output);
        job->session = ext_image_copy_capture_manager_v1_create_session(copy_manager, job->source, 0);
        ext_image_copy_capture_session_v1_add_listener(job->session, &SESSION_LISTENER, job);
    }

} // namespace fenriz::bar
