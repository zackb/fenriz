#pragma once

#include <gtk/gtk.h>

#include <map>
#include <string>

namespace fenriz::desktop {

    // Keyboard cleaning mode.
    class Cleaning {
    public:
        ~Cleaning();

        Cleaning() = default;
        Cleaning(const Cleaning&) = delete;
        Cleaning& operator=(const Cleaning&) = delete;

        // Watch the compositor's event feed for cleaning start/stop.
        void start(GtkApplication* app);

        // Ask, then run `fenrizctl cleaning`.
        void confirm();

    private:
        void show(int seconds, const std::string& cancel);
        void hide();
        void add_monitor(GdkMonitor* monitor, const std::string& cancel);
        void update_labels();

        void read_line();
        static void on_line(GObject* source, GAsyncResult* res, gpointer data);
        static gboolean on_tick(gpointer data);
        static void on_confirm_response(GtkWidget* button, gpointer data);
        static gboolean on_confirm_key(
            GtkEventControllerKey* keys, guint keyval, guint keycode, GdkModifierType state, gpointer data);

        GtkApplication* app_ = nullptr;
        GSocketConnection* conn_ = nullptr;
        GDataInputStream* in_ = nullptr;

        GtkWindow* confirm_ = nullptr;
        std::map<GdkMonitor*, GtkWindow*> surfaces_;
        std::map<GdkMonitor*, GtkWidget*> times_;
        int remaining_ = 0;
        guint tick_ = 0;
    };

} // namespace fenriz::desktop
