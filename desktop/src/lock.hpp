#pragma once

#include <gtk/gtk.h>
#include <gtk4-session-lock.h>

#include <functional>
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

        // Called when the session unlocks, to undo an idle blank the unlock itself cannot.
        void set_wake_screens(std::function<void()> fn) { wake_screens_ = std::move(fn); }

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
        void arm_passive(bool persistent_only = false);
        void release_sleep_inhibitor();
        void tick();
        void set_error(const std::string& text);
        void set_status(const std::string& text);
        void set_busy(bool busy);

        static void on_monitor(GtkSessionLockInstance* self, GdkMonitor* monitor, gpointer data);
        static void on_locked(GtkSessionLockInstance* self, gpointer data);
        static void on_failed(GtkSessionLockInstance* self, gpointer data);
        static void on_unlocked(GtkSessionLockInstance* self, gpointer data);
        static void on_entry_activate(GtkWidget* entry, gpointer data);
        static void on_entry_changed(GtkEditable* entry, gpointer data);
        static gboolean on_tick(gpointer data);
        static gboolean
            on_key_pressed(GtkEventControllerKey* c, guint keyval, guint keycode, GdkModifierType s, gpointer data);
        static void on_motion(GtkEventControllerMotion* c, double x, double y, gpointer data);
        static void on_prepare_for_sleep(GDBusConnection* bus,
                                         const char* sender,
                                         const char* path,
                                         const char* iface,
                                         const char* signal,
                                         GVariant* params,
                                         gpointer data);
        static gboolean on_wake_arm(gpointer data);

        const Config& cfg_;
        Authenticator auth_;
        std::function<void()> wake_screens_;
        GtkSessionLockInstance* instance_ = nullptr;
        std::vector<Surface> surfaces_;
        guint tick_id_ = 0;
        GDBusConnection* system_bus_ = nullptr; // logind, for the resume-from-suspend signal
        guint sleep_sub_ = 0;
        guint wake_arm_id_ = 0;
        int sleep_fd_ = -1;
        bool suspend_pending_ = false;
        bool locked_ = false;
    };

} // namespace fenriz::desktop
