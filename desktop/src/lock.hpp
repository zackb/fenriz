#pragma once

#include <gtk/gtk.h>
#include <gtk4-session-lock.h>

#include <string>
#include <vector>

#include "auth.hpp"
#include "config.hpp"

namespace fenriz::desktop {

    // ext-session-lock-v1 screen lock
    class Lock {
    public:
        explicit Lock(const Config& cfg);
        ~Lock();

        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

        // Idempotent: locking while locked does nothing.
        void engage();
        bool active() const;

    private:
        // One per monitor
        struct Surface {
            GtkWidget* clock = nullptr;
            GtkWidget* date = nullptr;
            GtkWidget* entry = nullptr;
            GtkWidget* error = nullptr;
        };

        void build_for_monitor(GdkMonitor* monitor);
        void submit(GtkWidget* entry);
        void tick();
        void set_error(const std::string& text);
        void set_busy(bool busy);

        static void on_monitor(GtkSessionLockInstance* self, GdkMonitor* monitor, gpointer data);
        static void on_locked(GtkSessionLockInstance* self, gpointer data);
        static void on_failed(GtkSessionLockInstance* self, gpointer data);
        static void on_unlocked(GtkSessionLockInstance* self, gpointer data);
        static void on_entry_activate(GtkWidget* entry, gpointer data);
        static void on_entry_changed(GtkEditable* entry, gpointer data);
        static gboolean on_tick(gpointer data);

        const Config& cfg_;
        Authenticator auth_;
        GtkSessionLockInstance* instance_ = nullptr;
        GtkCssProvider* css_ = nullptr;
        std::vector<Surface> surfaces_;
        guint tick_id_ = 0;
        bool locked_ = false;
    };

} // namespace fenriz::desktop
