#include "screensaver.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace fenriz::desktop {

    namespace {

        // Both names serve the same method set only the interface name differs.
        constexpr const char* INTROSPECTION_XML = R"(<node>
  <interface name='org.freedesktop.ScreenSaver'>
    <method name='Inhibit'>
      <arg type='s' name='application_name' direction='in'/>
      <arg type='s' name='reason_for_inhibit' direction='in'/>
      <arg type='u' name='cookie' direction='out'/>
    </method>
    <method name='UnInhibit'>
      <arg type='u' name='cookie' direction='in'/>
    </method>
    <method name='GetActive'>
      <arg type='b' name='active' direction='out'/>
    </method>
    <method name='SetActive'>
      <arg type='b' name='e' direction='in'/>
      <arg type='b' name='active' direction='out'/>
    </method>
    <method name='GetActiveTime'>
      <arg type='u' name='seconds' direction='out'/>
    </method>
    <method name='GetSessionIdleTime'>
      <arg type='u' name='seconds' direction='out'/>
    </method>
    <method name='SimulateUserActivity'/>
    <method name='Lock'/>
    <signal name='ActiveChanged'>
      <arg type='b' name='new_value'/>
    </signal>
  </interface>
  <interface name='org.gnome.ScreenSaver'>
    <method name='Inhibit'>
      <arg type='s' name='application_name' direction='in'/>
      <arg type='s' name='reason_for_inhibit' direction='in'/>
      <arg type='u' name='cookie' direction='out'/>
    </method>
    <method name='UnInhibit'>
      <arg type='u' name='cookie' direction='in'/>
    </method>
    <method name='GetActive'>
      <arg type='b' name='active' direction='out'/>
    </method>
    <method name='SetActive'>
      <arg type='b' name='e' direction='in'/>
      <arg type='b' name='active' direction='out'/>
    </method>
    <method name='GetActiveTime'>
      <arg type='u' name='seconds' direction='out'/>
    </method>
    <method name='GetSessionIdleTime'>
      <arg type='u' name='seconds' direction='out'/>
    </method>
    <method name='SimulateUserActivity'/>
    <method name='Lock'/>
    <signal name='ActiveChanged'>
      <arg type='b' name='new_value'/>
    </signal>
  </interface>
</node>)";

        constexpr const char* FREEDESKTOP_NAME = "org.freedesktop.ScreenSaver";
        constexpr const char* GNOME_NAME = "org.gnome.ScreenSaver";

        void on_method_call(GDBusConnection* bus,
                            const gchar* sender,
                            const gchar*,
                            const gchar*,
                            const gchar* method,
                            GVariant* params,
                            GDBusMethodInvocation* invocation,
                            gpointer data) {
            static_cast<Screensaver*>(data)->handle_call(bus, sender, method, params, invocation);
        }

        const GDBusInterfaceVTable VTABLE = {on_method_call, nullptr, nullptr, {nullptr}};

        void bus_acquired_cb(GDBusConnection* bus, const gchar* name, gpointer data) {
            static_cast<Screensaver*>(data)->on_bus_acquired(bus, name);
        }

        void name_acquired_cb(GDBusConnection*, const gchar* name, gpointer) {
            g_message("screensaver: owning %s", name);
        }

        void name_lost_cb(GDBusConnection*, const gchar* name, gpointer) {
            g_warning("screensaver: could not own %s, another idle daemon holds it "
                      "(hypridle, quickshell/fenrizd, xfce4-power-manager?); DBus idle "
                      "inhibitors will not reach fenriz-desktop",
                      name);
        }

        void on_name_owner_changed(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer data) {
            const char* name = nullptr;
            const char* old_owner = nullptr;
            const char* new_owner = nullptr;
            g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
            if (new_owner && *new_owner)
                return; // name changed hands rather than disappearing
            static_cast<Screensaver*>(data)->drop_peer(name);
        }

    } // namespace

    bool screensaver_should_inhibit(std::string_view reason) {
        std::string lower(reason);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return lower.find("audio") == std::string::npos || lower.find("video") != std::string::npos;
    }

    Screensaver::Screensaver(Handler on_change) : on_change_(std::move(on_change)) {}

    Screensaver::~Screensaver() {
        for (guint id : owner_ids_)
            g_bus_unown_name(id);
        if (bus_ && name_watch_)
            g_dbus_connection_signal_unsubscribe(bus_, name_watch_);
        if (introspection_)
            g_dbus_node_info_unref(introspection_);
    }

    void Screensaver::start() {
        GError* err = nullptr;
        introspection_ = g_dbus_node_info_new_for_xml(INTROSPECTION_XML, &err);
        if (!introspection_) {
            g_warning("screensaver: bad introspection XML: %s", err ? err->message : "unknown");
            g_clear_error(&err);
            return;
        }
        for (const char* name : {FREEDESKTOP_NAME, GNOME_NAME})
            owner_ids_.push_back(g_bus_own_name(G_BUS_TYPE_SESSION,
                                                name,
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                bus_acquired_cb,
                                                name_acquired_cb,
                                                name_lost_cb,
                                                this,
                                                nullptr));
    }

    void Screensaver::on_bus_acquired(GDBusConnection* bus, const char* name) {
        const bool gnome = g_strcmp0(name, GNOME_NAME) == 0;
        GDBusInterfaceInfo* iface = g_dbus_node_info_lookup_interface(introspection_, name);
        // Apps look for the freedesktop interface under both of its historical paths.
        const char* paths_fd[] = {"/ScreenSaver", "/org/freedesktop/ScreenSaver", nullptr};
        const char* paths_gnome[] = {"/org/gnome/ScreenSaver", nullptr};
        for (const char** p = gnome ? paths_gnome : paths_fd; *p; ++p) {
            GError* err = nullptr;
            if (!g_dbus_connection_register_object(bus, *p, iface, &VTABLE, this, nullptr, &err)) {
                g_warning("screensaver: register %s on %s: %s", name, *p, err ? err->message : "unknown");
                g_clear_error(&err);
            }
        }

        if (name_watch_)
            return; // both names share one connection, so one subscription covers them
        bus_ = bus;
        name_watch_ = g_dbus_connection_signal_subscribe(bus,
                                                         "org.freedesktop.DBus",
                                                         "org.freedesktop.DBus",
                                                         "NameOwnerChanged",
                                                         "/org/freedesktop/DBus",
                                                         nullptr,
                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                         on_name_owner_changed,
                                                         this,
                                                         nullptr);
    }

    void Screensaver::drop_peer(const char* peer) {
        if (!peer)
            return;
        const gsize before = inhibitors_.size();
        for (auto it = inhibitors_.begin(); it != inhibitors_.end();)
            it = it->second == peer ? inhibitors_.erase(it) : std::next(it);
        if (inhibitors_.size() == before)
            return;
        g_message("screensaver: %s left the bus, dropped %zu inhibitor(s)", peer, before - inhibitors_.size());
        notify();
    }

    void Screensaver::notify() {
        const bool inhibited = !inhibitors_.empty();
        if (inhibited == inhibited_)
            return;
        inhibited_ = inhibited;
        if (on_change_)
            on_change_(inhibited);
    }

    void Screensaver::handle_call(
        GDBusConnection*, const char* sender, const char* method, GVariant* params, GDBusMethodInvocation* invocation) {
        if (g_strcmp0(method, "Inhibit") == 0) {
            const char* app = nullptr;
            const char* reason = nullptr;
            g_variant_get(params, "(&s&s)", &app, &reason);
            if (!app || !*app) {
                g_dbus_method_invocation_return_error_literal(
                    invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "application name required");
                return;
            }
            if (!screensaver_should_inhibit(reason ? reason : "")) {
                g_message("screensaver: ignoring audio-only inhibit from %s: \"%s\"", app, reason);
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0u));
                return;
            }
            const guint32 cookie = ++counter_;
            inhibitors_[cookie] = sender ? sender : "";
            g_message("screensaver: inhibited by %s (%s): \"%s\" -> %u", app, sender, reason, cookie);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", cookie));
            notify();
            return;
        }
        if (g_strcmp0(method, "UnInhibit") == 0) {
            guint32 cookie = 0;
            g_variant_get(params, "(u)", &cookie);
            const bool erased = inhibitors_.erase(cookie) > 0;
            g_dbus_method_invocation_return_value(invocation, nullptr);
            if (erased)
                notify();
            return;
        }
        // We broker inhibitors rather than running a screensaver; the rest are stubs so
        // introspection-driven callers do not trip over a missing method.
        if (g_strcmp0(method, "GetActive") == 0 || g_strcmp0(method, "SetActive") == 0)
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        else if (g_strcmp0(method, "GetActiveTime") == 0 || g_strcmp0(method, "GetSessionIdleTime") == 0)
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0u));
        else
            g_dbus_method_invocation_return_value(invocation, nullptr);
    }

} // namespace fenriz::desktop
