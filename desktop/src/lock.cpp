#include "lock.hpp"

#include <gio/gunixfdlist.h>
#include <unistd.h>

#ifndef FENRIZ_DESKTOP_DATADIR
#define FENRIZ_DESKTOP_DATADIR "/usr/share/fenriz-desktop"
#endif

namespace fenriz::desktop {

    namespace {

        // A fingerprint reader does not survive a suspend, and the USB side of it needs a moment
        // to come back before PAM can claim it. TODO: probably needs tuning
        constexpr int WAKE_ARM_DELAY_SECONDS = 2;

        // logind "delay" inhibitor.
        // so the lock surface is on screen before the display sleeps rather than a moment after
        // it wakes. logind cuts us off at InhibitDelayMaxSec (5s by default) regardless.
        int take_sleep_inhibitor(GDBusConnection* bus) {
            GUnixFDList* fds = nullptr;
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
                bus,
                "org.freedesktop.login1",
                "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager",
                "Inhibit",
                g_variant_new("(ssss)", "sleep", "fenriz-desktop", "Locking the session", "delay"),
                G_VARIANT_TYPE("(h)"),
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &fds,
                nullptr,
                &error);
            if (!reply) {
                g_warning("lock: no sleep inhibitor: %s", error->message);
                g_error_free(error);
                return -1;
            }

            gint index = -1;
            g_variant_get(reply, "(h)", &index);
            int fd = fds ? g_unix_fd_list_get(fds, index, nullptr) : -1;
            g_variant_unref(reply);
            g_clear_object(&fds);
            return fd;
        }

    } // namespace

    Lock::Lock(const Config& cfg) : cfg_(cfg) {
        system_bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
        if (!system_bus_)
            return;
        sleep_sub_ = g_dbus_connection_signal_subscribe(system_bus_,
                                                        "org.freedesktop.login1",
                                                        "org.freedesktop.login1.Manager",
                                                        "PrepareForSleep",
                                                        "/org/freedesktop/login1",
                                                        nullptr,
                                                        G_DBUS_SIGNAL_FLAGS_NONE,
                                                        on_prepare_for_sleep,
                                                        this,
                                                        nullptr);
        if (cfg_.lock_on_suspend)
            sleep_fd_ = take_sleep_inhibitor(system_bus_);
    }

    Lock::~Lock() {
        if (tick_id_)
            g_source_remove(tick_id_);
        if (wake_arm_id_)
            g_source_remove(wake_arm_id_);
        if (sleep_sub_)
            g_dbus_connection_signal_unsubscribe(system_bus_, sleep_sub_);
        release_sleep_inhibitor();
        g_clear_object(&system_bus_);
        g_clear_object(&instance_);
    }

    bool Lock::active() const { return locked_; }

    void Lock::engage() {
        if (locked_)
            return;
        if (!gtk_session_lock_is_supported()) {
            g_warning("lock: compositor does not support ext-session-lock-v1");
            return;
        }
        // Locking with no PAM service would hand out a prompt that can never say yes.
        if (!pam_service_installed(auth_.service())) {
            g_warning("lock: refusing to lock, no PAM service '%s' installed — nothing could "
                      "unlock it. Install it with:\n"
                      "  sudo install -m644 %s/pam/%s /etc/pam.d/",
                      auth_.service().c_str(),
                      FENRIZ_DESKTOP_DATADIR,
                      auth_.service().c_str());
            return;
        }

        surfaces_.clear();
        instance_ = gtk_session_lock_instance_new();
        g_signal_connect(instance_, "monitor", G_CALLBACK(on_monitor), this);
        g_signal_connect(instance_, "locked", G_CALLBACK(on_locked), this);
        g_signal_connect(instance_, "failed", G_CALLBACK(on_failed), this);
        g_signal_connect(instance_, "unlocked", G_CALLBACK(on_unlocked), this);

        if (!gtk_session_lock_instance_lock(instance_)) {
            g_warning("lock: the compositor refused the lock");
            g_clear_object(&instance_);
            return;
        }
        locked_ = true;
        if (!tick_id_)
            tick_id_ = g_timeout_add_seconds(1, on_tick, this);
        tick();

        arm_passive();
    }

    void Lock::arm_passive(bool persistent_only) {
        if (!locked_)
            return;
        auth_.begin_passive(
            [this](bool ok, std::string) {
                if (ok && instance_)
                    gtk_session_lock_instance_unlock(instance_);
            },
            [this](std::string message) { set_status(message); },
            persistent_only);
    }

    void Lock::release_sleep_inhibitor() {
        if (sleep_fd_ < 0)
            return;
        close(sleep_fd_);
        sleep_fd_ = -1;
    }

    void Lock::on_prepare_for_sleep(
        GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* params, gpointer data) {
        gboolean sleeping = TRUE;
        g_variant_get(params, "(b)", &sleeping);
        auto* self = static_cast<Lock*>(data);

        if (!sleeping) { // resuming
            if (self->cfg_.lock_on_suspend && self->sleep_fd_ < 0)
                self->sleep_fd_ = take_sleep_inhibitor(self->system_bus_);
            if (self->locked_ && !self->wake_arm_id_)
                self->wake_arm_id_ = g_timeout_add_seconds(WAKE_ARM_DELAY_SECONDS, on_wake_arm, self);
            return;
        }

        // closing the lid is a request to lock, whatever the idle timers are doing
        if (!self->cfg_.lock_on_suspend)
            return;
        if (self->locked_) {
            self->release_sleep_inhibitor(); // nothing to wait for
            return;
        }
        self->suspend_pending_ = true;
        self->engage();
        if (!self->locked_) { // refused: never stall a suspend over a lock that cannot happen
            self->suspend_pending_ = false;
            self->release_sleep_inhibitor();
        }
    }

    gboolean Lock::on_wake_arm(gpointer data) {
        auto* self = static_cast<Lock*>(data);
        self->wake_arm_id_ = 0;
        self->arm_passive();
        return G_SOURCE_REMOVE;
    }

    gboolean Lock::on_key_pressed(GtkEventControllerKey*, guint, guint, GdkModifierType, gpointer data) {
        static_cast<Lock*>(data)->arm_passive();
        return FALSE; // observing only; the entry still gets the key
    }

    void Lock::on_motion(GtkEventControllerMotion*, double, double, gpointer data) {
        static_cast<Lock*>(data)->arm_passive();
    }

    void Lock::on_monitor(GtkSessionLockInstance*, GdkMonitor* monitor, gpointer data) {
        static_cast<Lock*>(data)->build_for_monitor(monitor);
    }

    void Lock::build_for_monitor(GdkMonitor* monitor) {
        GtkWindow* window = GTK_WINDOW(gtk_window_new());

        GtkWidget* overlay = gtk_overlay_new();

        const char* connector = gdk_monitor_get_connector(monitor);
        const std::string& path = cfg_.wallpaper_for(connector ? connector : "");
        if (!path.empty()) {
            GdkTexture* texture = gdk_texture_new_from_filename(path.c_str(), nullptr);
            if (texture) {
                GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
                g_object_unref(texture);
                gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
                gtk_widget_add_css_class(picture, "lock-wallpaper");
                gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);
            }
        }

        GtkWidget* scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(scrim, "lock-scrim");

        GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_halign(column, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(column, TRUE);

        Surface s;
        s.clock = gtk_label_new("");
        gtk_widget_add_css_class(s.clock, "lock-clock");
        gtk_box_append(GTK_BOX(column), s.clock);

        s.date = gtk_label_new("");
        gtk_widget_add_css_class(s.date, "lock-date");
        gtk_widget_set_margin_bottom(s.date, 26);
        gtk_box_append(GTK_BOX(column), s.date);

        s.entry = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(s.entry), FALSE);
        g_object_set(s.entry, "placeholder-text", "Password", nullptr);
        gtk_widget_add_css_class(s.entry, "fenriz-field");
        gtk_widget_add_css_class(s.entry, "lock-entry");
        gtk_editable_set_alignment(GTK_EDITABLE(s.entry), 0.5f);
        gtk_widget_set_size_request(s.entry, 280, -1);
        gtk_widget_set_halign(s.entry, GTK_ALIGN_CENTER);
        g_signal_connect(s.entry, "activate", G_CALLBACK(on_entry_activate), this);
        g_signal_connect(s.entry, "changed", G_CALLBACK(on_entry_changed), this);
        gtk_box_append(GTK_BOX(column), s.entry);

        s.error = gtk_label_new("");
        gtk_widget_add_css_class(s.error, "lock-error");
        gtk_box_append(GTK_BOX(column), s.error);

        gtk_box_append(GTK_BOX(scrim), column);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scrim);
        gtk_window_set_child(window, overlay);
        gtk_window_set_focus(window, s.entry);

        // Someone is at the machine: offer the reader again.
        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), this);
        gtk_widget_add_controller(GTK_WIDGET(window), keys);

        GtkEventController* motion = gtk_event_controller_motion_new();
        gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
        g_signal_connect(motion, "motion", G_CALLBACK(on_motion), this);
        gtk_widget_add_controller(GTK_WIDGET(window), motion);

        surfaces_.push_back(s);
        gtk_session_lock_instance_assign_window_to_monitor(instance_, window, monitor);
        tick();
    }

    void Lock::on_entry_activate(GtkWidget* entry, gpointer data) { static_cast<Lock*>(data)->submit(entry); }

    // typing again drops the failure state, so the red border tracks the current attempt
    void Lock::on_entry_changed(GtkEditable* entry, gpointer data) {
        const char* text = gtk_editable_get_text(entry);
        if (text && *text)
            static_cast<Lock*>(data)->set_error("");
    }

    void Lock::submit(GtkWidget* entry) {
        if (auth_.busy())
            return;
        const char* text = gtk_editable_get_text(GTK_EDITABLE(entry));
        set_error("");
        set_busy(true);

        auth_.submit_password(text ? text : "", [this](bool ok, std::string message) {
            set_busy(false);
            for (Surface& s : surfaces_)
                if (s.entry)
                    gtk_editable_set_text(GTK_EDITABLE(s.entry), "");
            if (ok) {
                if (instance_)
                    gtk_session_lock_instance_unlock(instance_);
                return;
            }
            // anything that is not an explicit success keeps the session locked
            set_error(message.empty() ? "Authentication failed" : message);
            // put the caret back
            for (Surface& s : surfaces_)
                if (s.entry && gtk_widget_get_root(s.entry))
                    gtk_window_set_focus(GTK_WINDOW(gtk_widget_get_root(s.entry)), s.entry);
        });
    }

    void Lock::set_busy(bool busy) {
        for (Surface& s : surfaces_)
            if (s.entry)
                gtk_editable_set_editable(GTK_EDITABLE(s.entry), !busy);
    }

    void Lock::set_error(const std::string& text) {
        for (Surface& s : surfaces_) {
            if (s.error) {
                gtk_label_set_text(GTK_LABEL(s.error), text.c_str());
                gtk_widget_remove_css_class(s.error, "status");
            }
            if (!s.entry)
                continue;
            if (text.empty())
                gtk_widget_remove_css_class(s.entry, "error");
            else
                gtk_widget_add_css_class(s.entry, "error");
        }
    }

    // What a passive method has to say ("Place your finger on the reader").
    void Lock::set_status(const std::string& text) {
        for (Surface& s : surfaces_) {
            if (!s.error)
                continue;
            gtk_label_set_text(GTK_LABEL(s.error), text.c_str());
            gtk_widget_add_css_class(s.error, "status");
        }
    }

    gboolean Lock::on_tick(gpointer data) {
        static_cast<Lock*>(data)->tick();
        return G_SOURCE_CONTINUE;
    }

    void Lock::tick() {
        if (!locked_)
            return;

        // pam_fprintd gives up on its own (timeout=30 max-tries=3 by default)
        arm_passive(true);

        GDateTime* now = g_date_time_new_now_local();
        char* time = g_date_time_format(now, "%-I:%M");
        char* date = g_date_time_format(now, "%A, %B %-d");
        for (Surface& s : surfaces_) {
            if (s.clock)
                gtk_label_set_text(GTK_LABEL(s.clock), time);
            if (s.date)
                gtk_label_set_text(GTK_LABEL(s.date), date);
        }
        g_free(time);
        g_free(date);
        g_date_time_unref(now);
    }

    void Lock::on_locked(GtkSessionLockInstance*, gpointer data) {
        auto* self = static_cast<Lock*>(data);
        // compositor has the lock surface up, sleep can proceed.
        if (self->suspend_pending_) {
            self->suspend_pending_ = false;
            self->release_sleep_inhibitor();
        }
        g_message("lock: session locked");
    }

    void Lock::on_failed(GtkSessionLockInstance*, gpointer data) {
        auto* self = static_cast<Lock*>(data);
        // compositor refused. Stay unlocked rather than pretending otherwise
        g_warning("lock: compositor refused to lock the session");
        self->locked_ = false;
        self->surfaces_.clear();
        g_clear_object(&self->instance_);
    }

    void Lock::on_unlocked(GtkSessionLockInstance*, gpointer data) {
        auto* self = static_cast<Lock*>(data);
        self->locked_ = false;
        if (self->wake_screens_)
            self->wake_screens_();
        self->auth_.cancel();
        self->surfaces_.clear(); // library already destroyed the windows
        if (self->tick_id_) {
            g_source_remove(self->tick_id_);
            self->tick_id_ = 0;
        }
        if (self->wake_arm_id_) {
            g_source_remove(self->wake_arm_id_);
            self->wake_arm_id_ = 0;
        }
        g_clear_object(&self->instance_);
        g_message("lock: session unlocked");
    }

} // namespace fenriz::desktop
