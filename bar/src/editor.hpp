#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

#include <functional>

namespace fenriz::bar {

    // Called with the annotated image, or nullptr when cancelled. The callee owns the surface.
    using EditorDone = std::function<void(cairo_surface_t*)>;

    // A full-screen annotation overlay on `monitor`: brush, line, arrow, box, ellipse and text over `image`
    // Enter finishes, Esc cancels, Ctrl+Z undoes.
    void open_editor(GtkApplication* app, GdkMonitor* monitor, cairo_surface_t* image, EditorDone done);

    // Disconnects every handler with `data` from the event controllers of `widget` and its descendants.
    void detach_handlers(GtkWidget* widget, gpointer data);

} // namespace fenriz::bar
