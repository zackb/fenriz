#pragma once

#include <gtk/gtk.h>

namespace fenriz::desktop {

    class Osd {
    public:
        ~Osd();

        Osd(const Osd&) = delete;
        Osd& operator=(const Osd&) = delete;
        Osd() = default;

        // `icon` is an icon theme name, `percent` is 0..100
        void show(GtkApplication* app, const char* icon, int percent);

    private:
        void build(GtkApplication* app);

        static gboolean on_fade(gpointer data);
        static gboolean on_faded(gpointer data);

        GtkWindow* window_ = nullptr;
        GtkWidget* pill_ = nullptr;
        GtkWidget* image_ = nullptr;
        GtkWidget* bar_ = nullptr;
        guint hide_id_ = 0;
    };

} // namespace fenriz::desktop
