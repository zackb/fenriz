#include "bluetooth.hpp"

#include <algorithm>
#include <memory>

namespace fenriz::bar {

    namespace {

        constexpr const char* AGENT_PATH = "/dev/fenriz/bar/bluetooth_agent";
        constexpr int CONNECT_TIMEOUT_MS = 30000;
        constexpr int PAIR_TIMEOUT_MS = 60000;

        constexpr const char* AGENT_XML = R"xml(
<node>
  <interface name="org.bluez.Agent1">
    <method name="Release"/>
    <method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
    <method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
    <method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
    <method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
    <method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
    <method name="RequestAuthorization"><arg type="o" direction="in"/></method>
    <method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
    <method name="Cancel"/>
  </interface>
</node>)xml";

        bool bool_prop(GDBusProxy* proxy, const char* name) {
            GVariant* v = g_dbus_proxy_get_cached_property(proxy, name);
            const bool b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
            if (v)
                g_variant_unref(v);
            return b;
        }

        std::string string_prop(GDBusProxy* proxy, const char* name) {
            GVariant* v = g_dbus_proxy_get_cached_property(proxy, name);
            std::string s;
            if (v &&
                (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
                s = g_variant_get_string(v, nullptr);
            if (v)
                g_variant_unref(v);
            return s;
        }

    } // namespace

    void sort_devices(std::vector<BtDevice>& devices) {
        std::stable_sort(devices.begin(), devices.end(), [](const BtDevice& a, const BtDevice& b) {
            if (a.paired != b.paired)
                return a.paired;
            if (a.paired) {
                if (a.connected != b.connected)
                    return a.connected;
                return g_utf8_collate(a.name.c_str(), b.name.c_str()) < 0;
            }
            return a.rssi > b.rssi;
        });
    }

    BtChanges bt_changes(const std::vector<BtDevice>& before, const std::vector<BtDevice>& after) {
        BtChanges c;
        for (const BtDevice& now : after) {
            auto was =
                std::find_if(before.begin(), before.end(), [&](const BtDevice& d) { return d.path == now.path; });
            const bool was_connected = was != before.end() && was->connected;
            if (now.connected && !was_connected)
                c.connected.push_back(now);
            else if (!now.connected && was_connected)
                c.disconnected.push_back(now);
        }
        // a device BlueZ dropped entirely while connected (adapter powered off) went away too
        for (const BtDevice& was : before)
            if (was.connected &&
                std::none_of(after.begin(), after.end(), [&](const BtDevice& d) { return d.path == was.path; }))
                c.disconnected.push_back(was);
        return c;
    }

    std::string bt_device_icon(const std::string& icon) {
        return icon.empty() ? "fenriz-bluetooth-symbolic" : icon + "-symbolic";
    }

    struct Bluetooth::Op {
        std::function<void(bool, const std::string&)> done;
    };

    Bluetooth::~Bluetooth() {
        if (cancel_) {
            g_cancellable_cancel(cancel_);
            g_object_unref(cancel_);
        }
        if (discovering_ && adapter_)
            g_dbus_proxy_call_sync(adapter_, "StopDiscovery", nullptr, G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
        if (agent_id_)
            g_dbus_connection_unregister_object(bus_, agent_id_);
        if (manager_) {
            g_signal_handlers_disconnect_by_data(manager_, this);
            g_object_unref(manager_);
        }
        g_clear_object(&adapter_);
        g_clear_object(&bus_);
    }

    void Bluetooth::start() {
        GError* err = nullptr;
        bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
        if (!bus_) {
            g_message("bluetooth: no system bus: %s", err->message);
            g_error_free(err);
            return;
        }
        cancel_ = g_cancellable_new();
        g_dbus_object_manager_client_new(bus_,
                                         G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                         "org.bluez",
                                         "/",
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         cancel_,
                                         on_manager,
                                         this);
    }

    void Bluetooth::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void Bluetooth::on_connection(std::function<void(const BtDevice&, bool)> listener) {
        connection_listeners_.push_back(std::move(listener));
    }

    void Bluetooth::on_error(std::function<void(const std::string&)> listener) {
        error_listeners_.push_back(std::move(listener));
    }

    void Bluetooth::on_manager(GObject*, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GDBusObjectManager* manager = g_dbus_object_manager_client_new_finish(res, &err);
        if (!manager) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_message("bluetooth: %s", err->message);
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Bluetooth*>(data);
        self->manager_ = manager;
        auto changed = G_CALLBACK(+[](Bluetooth* bt) { bt->refresh(); });
        for (const char* signal : {"object-added", "object-removed", "interface-added", "interface-removed"})
            g_signal_connect_swapped(manager, signal, changed, self);
        g_signal_connect_swapped(manager, "interface-proxy-properties-changed", changed, self);
        // bluetoothd restarting forgets our agent
        g_signal_connect_swapped(manager,
                                 "notify::name-owner",
                                 G_CALLBACK(+[](Bluetooth* bt) {
                                     bt->register_agent();
                                     bt->refresh();
                                 }),
                                 self);
        self->register_agent();
        self->refresh();
    }

    void Bluetooth::refresh() {
        g_clear_object(&adapter_);
        std::vector<BtDevice> devices;
        GList* objects = g_dbus_object_manager_get_objects(manager_);
        for (GList* l = objects; l; l = l->next) {
            GDBusObject* obj = G_DBUS_OBJECT(l->data);
            if (!adapter_)
                if (GDBusInterface* a = g_dbus_object_get_interface(obj, "org.bluez.Adapter1"))
                    adapter_ = G_DBUS_PROXY(a);
        }
        const std::string adapter_path = adapter_ ? g_dbus_proxy_get_object_path(adapter_) : "";
        for (GList* l = objects; l && adapter_; l = l->next) {
            GDBusObject* obj = G_DBUS_OBJECT(l->data);
            GDBusInterface* iface = g_dbus_object_get_interface(obj, "org.bluez.Device1");
            if (!iface)
                continue;
            GDBusProxy* p = G_DBUS_PROXY(iface);
            if (string_prop(p, "Adapter") == adapter_path) {
                BtDevice d;
                d.path = g_dbus_object_get_object_path(obj);
                d.name = string_prop(p, "Alias");
                d.has_name = !string_prop(p, "Name").empty();
                d.icon = string_prop(p, "Icon");
                d.paired = bool_prop(p, "Paired");
                d.trusted = bool_prop(p, "Trusted");
                d.connected = bool_prop(p, "Connected");
                if (GVariant* rssi = g_dbus_proxy_get_cached_property(p, "RSSI")) {
                    if (g_variant_is_of_type(rssi, G_VARIANT_TYPE_INT16))
                        d.rssi = g_variant_get_int16(rssi);
                    g_variant_unref(rssi);
                }
                if (GDBusInterface* bat = g_dbus_object_get_interface(obj, "org.bluez.Battery1")) {
                    if (GVariant* v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bat), "Percentage")) {
                        if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
                            d.battery = g_variant_get_byte(v);
                        g_variant_unref(v);
                    }
                    g_object_unref(bat);
                }
                d.busy = std::find(busy_.begin(), busy_.end(), d.path) != busy_.end();
                devices.push_back(std::move(d));
            }
            g_object_unref(iface);
        }
        g_list_free_full(objects, g_object_unref);

        const bool was_powered = powered_;
        powered_ = adapter_ && bool_prop(adapter_, "Powered");
        // BlueZ refuses discovery while off, so a scan the page asked for then starts now
        if (powered_ && !was_powered && discover_wanted_ > 0)
            call(
                g_dbus_proxy_get_object_path(adapter_), "org.bluez.Adapter1", "StartDiscovery", nullptr, 5000, nullptr);
        discovering_ = adapter_ && bool_prop(adapter_, "Discovering");
        sort_devices(devices);

        const BtChanges changes = bt_changes(devices_, devices);
        devices_ = std::move(devices);
        if (seen_)
            for (const auto& listener : connection_listeners_) {
                for (const BtDevice& d : changes.connected)
                    listener(d, true);
                for (const BtDevice& d : changes.disconnected)
                    listener(d, false);
            }
        seen_ = manager_ != nullptr;
        for (auto& listener : listeners_)
            listener();
    }

    void Bluetooth::call(const std::string& path,
                         const char* iface,
                         const char* method,
                         GVariant* args,
                         int timeout_ms,
                         std::function<void(bool, const std::string&)> done) {
        g_dbus_connection_call(
            bus_,
            "org.bluez",
            path.c_str(),
            iface,
            method,
            args,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            timeout_ms,
            cancel_,
            [](GObject* source, GAsyncResult* res, gpointer data) {
                std::unique_ptr<Op> op(static_cast<Op*>(data));
                GError* err = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
                if (reply)
                    g_variant_unref(reply);
                if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    g_error_free(err);
                    return;
                }
                std::string message;
                if (err) {
                    g_dbus_error_strip_remote_error(err);
                    message = err->message;
                    g_error_free(err);
                }
                if (op->done)
                    op->done(reply != nullptr, message);
            },
            new Op{std::move(done)});
    }

    void Bluetooth::set_busy(const std::string& path, bool busy) {
        std::erase(busy_, path);
        if (busy)
            busy_.push_back(path);
        if (manager_)
            refresh();
    }

    void Bluetooth::set_powered(bool on) {
        if (!adapter_)
            return;
        call(g_dbus_proxy_get_object_path(adapter_),
             "org.freedesktop.DBus.Properties",
             "Set",
             g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(on)),
             5000,
             [this, on](bool ok, const std::string& error) {
                 if (!ok)
                     for (auto& l : error_listeners_)
                         l(std::string("Couldn't turn Bluetooth ") + (on ? "on" : "off") + ": " + error);
             });
    }

    void Bluetooth::discover(bool on) {
        const int before = discover_wanted_;
        discover_wanted_ = std::max(0, discover_wanted_ + (on ? 1 : -1));
        if (!adapter_ || (before > 0) == (discover_wanted_ > 0))
            return;
        // "already discovering" from someone else's scan is fine either way
        call(g_dbus_proxy_get_object_path(adapter_),
             "org.bluez.Adapter1",
             discover_wanted_ > 0 ? "StartDiscovery" : "StopDiscovery",
             nullptr,
             5000,
             nullptr);
    }

    void Bluetooth::connect(const std::string& path) {
        set_busy(path, true);
        call(path,
             "org.bluez.Device1",
             "Connect",
             nullptr,
             CONNECT_TIMEOUT_MS,
             [this, path](bool ok, const std::string& e) {
                 set_busy(path, false);
                 if (!ok)
                     for (auto& l : error_listeners_)
                         l("Couldn't connect: " + e);
             });
    }

    void Bluetooth::disconnect(const std::string& path) {
        set_busy(path, true);
        call(path,
             "org.bluez.Device1",
             "Disconnect",
             nullptr,
             CONNECT_TIMEOUT_MS,
             [this, path](bool, const std::string&) { set_busy(path, false); });
    }

    void Bluetooth::pair(const std::string& path) {
        pairing_ = path;
        set_busy(path, true);
        call(path, "org.bluez.Device1", "Pair", nullptr, PAIR_TIMEOUT_MS, [this, path](bool ok, const std::string& e) {
            if (!ok) {
                pairing_.clear();
                set_busy(path, false);
                for (auto& l : error_listeners_)
                    l("Couldn't pair: " + e);
                return;
            }
            // Trusted, so it reconnects on its own later. Until that lands the agent still authorizes this device's
            // services, which BlueZ asks about on the first connect of an untrusted device.
            call(path,
                 "org.freedesktop.DBus.Properties",
                 "Set",
                 g_variant_new("(ssv)", "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE)),
                 5000,
                 [this, path](bool, const std::string&) {
                     pairing_.clear();
                     set_busy(path, false);
                     connect(path);
                 });
        });
    }

    void Bluetooth::forget(const std::string& path) {
        if (!adapter_)
            return;
        call(g_dbus_proxy_get_object_path(adapter_),
             "org.bluez.Adapter1",
             "RemoveDevice",
             g_variant_new("(o)", path.c_str()),
             5000,
             nullptr);
    }

    // A NoInputNoOutput agent: it can neither show nor type a code, so it only agrees to what we asked for ourselves.
    // Pairing a device that needs a PIN or passkey (most keyboards) is refused rather than silently accepted.
    void Bluetooth::register_agent() {
        if (!agent_id_) {
            GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(AGENT_XML, nullptr);
            static const GDBusInterfaceVTable vtable = {
                +[](GDBusConnection*,
                    const char*,
                    const char*,
                    const char*,
                    const char* method,
                    GVariant* params,
                    GDBusMethodInvocation* invocation,
                    gpointer data) {
                    auto* self = static_cast<Bluetooth*>(data);
                    const std::string m = method;
                    if (m == "Release" || m == "Cancel") {
                        g_dbus_method_invocation_return_value(invocation, nullptr);
                        return;
                    }
                    const char* device = nullptr;
                    if (m == "RequestConfirmation" || m == "RequestAuthorization" || m == "AuthorizeService") {
                        g_variant_get_child(params, 0, "&o", &device);
                        if (device && self->pairing_ == device) {
                            g_dbus_method_invocation_return_value(invocation, nullptr);
                            return;
                        }
                    }
                    g_dbus_method_invocation_return_dbus_error(
                        invocation, "org.bluez.Error.Rejected", "fenriz-bar cannot show or enter a pairing code");
                },
                nullptr,
                nullptr,
                {}};
            GError* err = nullptr;
            agent_id_ =
                g_dbus_connection_register_object(bus_, AGENT_PATH, info->interfaces[0], &vtable, this, nullptr, &err);
            g_dbus_node_info_unref(info);
            if (!agent_id_) {
                g_warning("bluetooth: agent: %s", err->message);
                g_error_free(err);
                return;
            }
        }
        call("/org/bluez",
             "org.bluez.AgentManager1",
             "RegisterAgent",
             g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"),
             5000,
             [](bool ok, const std::string& error) {
                 if (!ok && error.find("AlreadyExists") == std::string::npos)
                     g_message("bluetooth: could not register the pairing agent: %s", error.c_str());
             });
    }

} // namespace fenriz::bar
