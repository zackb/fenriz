#pragma once

#include <gio/gio.h>

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

    private:
        guint32 notify(GVariant* params);
        void emit(const char* signal, GVariant* params);

        GtkApplication* app_;
        int default_timeout_;
        Toasts toasts_;
        GDBusNodeInfo* introspection_ = nullptr;
        GDBusConnection* bus_ = nullptr;
        guint owner_id_ = 0;
        guint32 counter_ = 0;
    };

} // namespace fenriz::desktop
