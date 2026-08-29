#pragma once

#include <gio/gio.h>

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "toast.hpp"

namespace fenriz::desktop {

    // How long a notification stays up, in milliseconds; 0 never expires.
    // The client's own timeout wins, then `default_ms`, except that a critical one is
    // sticky unless the client explicitly asked for a deadline.
    int notify_expiry_ms(int client_timeout, guint8 urgency, int default_ms);

    // Bodies may have a pango-markup subset, but senders get it wrong often enough that a
    // malformed one would render blank.
    std::string notify_body_markup(const std::string& body);

    struct Actions {
        // key -> label, in the order given, minus "default"
        std::vector<std::pair<std::string, std::string>> buttons;
        bool has_default = false;
    };

    // The wire format is a flat [key, label, key, label, ...].
    Actions notify_split_actions(const std::vector<std::string>& flat);

    // Reads a hint the spec calls boolean. Senders variously send a byte, an int or "1"/"true".
    bool notify_hint_bool(GVariant* hints, const char* key);

    // A notification the panel can still show after its toast is gone.
    struct HistoryItem {
        guint32 id = 0;
        std::string app_name;
        std::string summary;
        std::string body; // valid pango markup, as the toast had it
        std::string icon;
        GdkTexture* texture = nullptr; // owned, or null
        bool critical = false;
        gint64 time = 0; // milliseconds since the epoch
    };

    // org.freedesktop.Notifications
    class Notifications {
    public:
        Notifications(GtkApplication* app, const Config& cfg);
        ~Notifications();

        Notifications(const Notifications&) = delete;
        Notifications& operator=(const Notifications&) = delete;

        void start();

        void handle_call(const char* method, GVariant* params, GDBusMethodInvocation* invocation);
        void on_bus_acquired(GDBusConnection* bus);

        // Newest first. Never persisted; capped at the config's notify_history.
        const std::deque<HistoryItem>& history() const { return history_; }
        void dismiss_history(guint32 id);
        void clear_history();
        // Fired after any change, so an open panel can redraw.
        void set_history_changed(std::function<void()> fn) { on_history_changed_ = std::move(fn); }

        // Records a notification the panel can show later. Public for the tests.
        void remember(const Toast& toast, const std::string& app_name);

    private:
        guint32 notify(GVariant* params);
        void emit(const char* signal, GVariant* params);

        GtkApplication* app_;
        int default_timeout_;
        size_t history_max_;
        Toasts toasts_;
        std::deque<HistoryItem> history_;
        std::function<void()> on_history_changed_;
        GDBusNodeInfo* introspection_ = nullptr;
        GDBusConnection* bus_ = nullptr;
        guint owner_id_ = 0;
        guint32 counter_ = 0;
    };

} // namespace fenriz::desktop
