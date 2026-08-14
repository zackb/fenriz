#pragma once

#include <gtk/gtk.h>

namespace fenriz::desktop::blur {

    // Which part of a surface the compositor should blur behind.
    enum class Mode {
        Widget,   // the bounds of one widget
        Children, // the bounds of each visible child of a widget
    };

    // Bind ext-background-effect-v1. False when the compositor lacks it or advertises
    // no blur capability.
    bool init();

    struct Band {
        int x, y, w, h;
    };
    static constexpr int MAX_BANDS = 5;

    // A w*h rounded rect with corner radius `r`, as horizontal bands every one of which is inscribed in the curve.
    int rounded_bands(int w, int h, int r, Band out[MAX_BANDS]);

    // Blur what is behind `widget` on this native's surface, re-applied whenever the surface resizes. `radius` is the
    // card's CSS corner radius.
    void attach(GtkNative* native, GtkWidget* widget, Mode mode, int radius);

} // namespace fenriz::desktop::blur
