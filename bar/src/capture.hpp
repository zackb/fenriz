#pragma once

#include <cairo.h>
#include <gdk/gdk.h>

#include <functional>

namespace fenriz::bar {

    // Called with the frame at native resolution, or nullptr on failure. The callee owns the surface.
    using CaptureDone = std::function<void(cairo_surface_t*)>;

    // False when the compositor lacks ext-image-copy-capture-v1 with output sources.
    bool capture_available();

    // One frame of `monitor`, over ext-image-copy-capture-v1 into shared memory. Asynchronous.
    void capture_output(GdkMonitor* monitor, CaptureDone done);

} // namespace fenriz::bar
