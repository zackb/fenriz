#include "lock.hpp"

namespace fenriz::desktop {

    namespace {

        // blur for wallpaper / lockscreen
        std::string css_for(const Config& cfg) {
            return ".lock-wallpaper { filter: blur(" + std::to_string(cfg.lock_blur) +
                   "px); }"
                   ".lock-scrim { background-color: rgba(0,0,0,0.45); }"
                   ".lock-clock { font-size: 76px; font-weight: 300; color: white; }"
                   ".lock-date  { font-size: 18px; color: alpha(white, 0.85); }"
                   ".lock-error { font-size: 14px; color: #ff8080; }";
        }

    } // namespace

    Lock::Lock(const Config& cfg) : cfg_(cfg) {}

    Lock::~Lock() {
        if (tick_id_)
            g_source_remove(tick_id_);
        g_clear_object(&css_);
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
                      "  sudo install -m644 /usr/share/fenriz-desktop/pam/%s /etc/pam.d/",
                      auth_.service().c_str(),
                      auth_.service().c_str());
            return;
        }

        if (!css_) {
            css_ = gtk_css_provider_new();
            gtk_css_provider_load_from_string(css_, css_for(cfg_).c_str());
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(), GTK_STYLE_PROVIDER(css_), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
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
        gtk_widget_set_size_request(s.entry, 280, -1);
        gtk_widget_set_halign(s.entry, GTK_ALIGN_CENTER);
        g_signal_connect(s.entry, "activate", G_CALLBACK(on_entry_activate), this);
        gtk_box_append(GTK_BOX(column), s.entry);

        s.error = gtk_label_new("");
        gtk_widget_add_css_class(s.error, "lock-error");
        gtk_box_append(GTK_BOX(column), s.error);

        gtk_box_append(GTK_BOX(scrim), column);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scrim);
        gtk_window_set_child(window, overlay);
        gtk_window_set_focus(window, s.entry);

        surfaces_.push_back(s);
        gtk_session_lock_instance_assign_window_to_monitor(instance_, window, monitor);
        tick();
    }

    void Lock::on_entry_activate(GtkWidget* entry, gpointer data) { static_cast<Lock*>(data)->submit(entry); }

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
        for (Surface& s : surfaces_)
            if (s.error)
                gtk_label_set_text(GTK_LABEL(s.error), text.c_str());
    }

    gboolean Lock::on_tick(gpointer data) {
        static_cast<Lock*>(data)->tick();
        return G_SOURCE_CONTINUE;
    }

    void Lock::tick() {
        if (!locked_)
            return;
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
        (void)data;
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
        self->auth_.cancel();
        self->surfaces_.clear(); // library already destroyed the windows
        if (self->tick_id_) {
            g_source_remove(self->tick_id_);
            self->tick_id_ = 0;
        }
        g_clear_object(&self->instance_);
        g_message("lock: session unlocked");
    }

} // namespace fenriz::desktop
