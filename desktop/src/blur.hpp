#pragma once

#include <gtk/gtk.h>

namespace fenriz::desktop::blur {

    // Bind ext-background-effect-v1. False when the compositor lacks it or advertises
    // no blur capability, after which attach() does nothing.
    bool init();

    // Ask the compositor to blur what is behind this native's surface. Safe before the surface is realized.
    void attach(GtkNative* native);

} // namespace fenriz::desktop::blur
