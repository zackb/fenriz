#include "network.hpp"

#include <algorithm>

namespace fenriz::bar {

    namespace {

        std::string ssid_of(NMAccessPoint* ap) {
            GBytes* bytes = nm_access_point_get_ssid(ap);
            if (!bytes)
                return "";
            gsize len = 0;
            const auto* data = static_cast<const guint8*>(g_bytes_get_data(bytes, &len));
            char* utf8 = nm_utils_ssid_to_utf8(data, len);
            std::string s = utf8 ? utf8 : "";
            g_free(utf8);
            return s;
        }

        bool activating(NMDeviceState s) { return s >= NM_DEVICE_STATE_PREPARE && s < NM_DEVICE_STATE_ACTIVATED; }

        // Finishes an async libnm call whose only interesting outcome is an error.
        template <typename Finish> std::string finish_error(Finish finish) {
            GError* err = nullptr;
            finish(&err);
            std::string message;
            if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                message = err->message;
            g_clear_error(&err);
            return message;
        }

    } // namespace

    WifiSecurity wifi_security(guint32 flags, guint32 wpa, guint32 rsn) {
        const guint32 keys = wpa | rsn;
        if (keys & (NM_802_11_AP_SEC_KEY_MGMT_802_1X | NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192))
            return WifiSecurity::Unsupported;
        if (keys & NM_802_11_AP_SEC_KEY_MGMT_PSK)
            return WifiSecurity::Psk; // includes WPA3 transition mode, where PSK still works
        if (rsn & NM_802_11_AP_SEC_KEY_MGMT_SAE)
            return WifiSecurity::Sae;
        if (rsn & (NM_802_11_AP_SEC_KEY_MGMT_OWE | NM_802_11_AP_SEC_KEY_MGMT_OWE_TM))
            return WifiSecurity::Open; // encrypted, but asks nothing of the user
        if (flags & NM_802_11_AP_FLAGS_PRIVACY)
            return WifiSecurity::Unsupported; // WEP
        return WifiSecurity::Open;
    }

    int signal_bars(int strength) {
        if (strength >= 80)
            return 4;
        if (strength >= 55)
            return 3;
        if (strength >= 30)
            return 2;
        if (strength >= 5)
            return 1;
        return 0;
    }

    const char* signal_icon(int strength) {
        static constexpr const char* ICONS[] = {"fenriz-wifi-strength-outline-symbolic",
                                                "fenriz-wifi-strength-1-symbolic",
                                                "fenriz-wifi-strength-2-symbolic",
                                                "fenriz-wifi-strength-3-symbolic",
                                                "fenriz-wifi-strength-4-symbolic"};
        return ICONS[signal_bars(strength)];
    }

    std::vector<WifiNetwork> merge_networks(std::vector<WifiNetwork> aps) {
        std::vector<WifiNetwork> out;
        for (WifiNetwork& ap : aps) {
            if (ap.ssid.empty())
                continue;
            auto it = std::find_if(out.begin(), out.end(), [&](const WifiNetwork& n) { return n.ssid == ap.ssid; });
            if (it == out.end()) {
                out.push_back(std::move(ap));
                continue;
            }
            const bool active = it->active || ap.active, connecting = it->connecting || ap.connecting;
            const bool saved = it->saved || ap.saved;
            // the access point in use wins over a stronger one we are not on
            if ((ap.active && !it->active) || (ap.active == it->active && ap.strength > it->strength))
                *it = std::move(ap);
            it->active = active;
            it->connecting = connecting;
            it->saved = saved;
        }
        std::stable_sort(out.begin(), out.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
            if (a.active != b.active)
                return a.active;
            if (a.saved != b.saved)
                return a.saved;
            if (signal_bars(a.strength) != signal_bars(b.strength))
                return signal_bars(a.strength) > signal_bars(b.strength);
            return g_utf8_collate(a.ssid.c_str(), b.ssid.c_str()) < 0;
        });
        return out;
    }

    Network::~Network() {
        if (cancel_) {
            g_cancellable_cancel(cancel_);
            g_object_unref(cancel_);
        }
        if (refresh_id_)
            g_source_remove(refresh_id_);
        for (NMDevice* d : watched_)
            g_signal_handlers_disconnect_by_data(d, this);
        if (client_) {
            g_signal_handlers_disconnect_by_data(client_, this);
            g_object_unref(client_);
        }
    }

    void Network::start() {
        cancel_ = g_cancellable_new();
        nm_client_new_async(cancel_, on_client, this);
    }

    void Network::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void Network::on_connected(std::function<void(const std::string&)> listener) {
        connected_listeners_.push_back(std::move(listener));
    }

    void Network::on_error(std::function<void(const std::string&, const std::string&)> listener) {
        error_listeners_.push_back(std::move(listener));
    }

    void Network::on_client(GObject*, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        NMClient* client = nm_client_new_finish(res, &err);
        if (!client) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_message("network: %s", err->message);
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Network*>(data);
        self->client_ = client;
        const auto changed = G_CALLBACK(+[](Network* n) { n->schedule_refresh(); });
        for (const char* signal : {"device-added", "device-removed"})
            g_signal_connect_swapped(client,
                                     signal,
                                     G_CALLBACK(+[](Network* n) {
                                         n->watch_devices();
                                         n->schedule_refresh();
                                     }),
                                     self);
        for (const char* signal : {"notify::nm-running",
                                   "notify::wireless-enabled",
                                   "notify::primary-connection",
                                   "connection-added",
                                   "connection-removed",
                                   "active-connection-added",
                                   "active-connection-removed"})
            g_signal_connect_swapped(client, signal, changed, self);
        self->watch_devices();
        self->refresh();
    }

    // libnm fires a burst of notifications for one change (a scan touches every access point); one refresh per burst.
    void Network::schedule_refresh() {
        if (!refresh_id_)
            refresh_id_ = g_idle_add(
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Network*>(data);
                    self->refresh_id_ = 0;
                    self->refresh();
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    void Network::watch_devices() {
        for (NMDevice* d : watched_)
            g_signal_handlers_disconnect_by_data(d, this);
        watched_.clear();
        wifi_ = nullptr;
        const GPtrArray* devices = nm_client_get_devices(client_);
        for (guint i = 0; devices && i < devices->len; i++) {
            auto* d = NM_DEVICE(g_ptr_array_index(devices, i));
            if (NM_IS_DEVICE_WIFI(d) && !wifi_) {
                wifi_ = NM_DEVICE_WIFI(d);
                for (const char* signal :
                     {"access-point-added", "access-point-removed", "notify::active-access-point", "notify::last-scan"})
                    g_signal_connect_swapped(d, signal, G_CALLBACK(+[](Network* n) { n->schedule_refresh(); }), this);
                g_signal_connect(d, "state-changed", G_CALLBACK(on_device_state), this);
            } else if (NM_IS_DEVICE_ETHERNET(d)) {
                g_signal_connect_swapped(
                    d, "notify::state", G_CALLBACK(+[](Network* n) { n->schedule_refresh(); }), this);
            } else {
                continue;
            }
            watched_.push_back(d);
        }
    }

    void Network::refresh() {
        enabled_ = running() && nm_client_wireless_get_enabled(client_);
        wired_ = false;
        const GPtrArray* devices = nm_client_get_devices(client_);
        for (guint i = 0; devices && i < devices->len; i++) {
            auto* d = NM_DEVICE(g_ptr_array_index(devices, i));
            if (NM_IS_DEVICE_ETHERNET(d) && nm_device_get_state(d) == NM_DEVICE_STATE_ACTIVATED)
                wired_ = true;
        }

        std::vector<WifiNetwork> aps;
        if (wifi_) {
            const NMDeviceState state = nm_device_get_state(NM_DEVICE(wifi_));
            NMAccessPoint* active = nm_device_wifi_get_active_access_point(wifi_);
            const GPtrArray* connections = nm_client_get_connections(client_);
            const GPtrArray* list = nm_device_wifi_get_access_points(wifi_);
            for (guint i = 0; list && i < list->len; i++) {
                auto* ap = NM_ACCESS_POINT(g_ptr_array_index(list, i));
                WifiNetwork n;
                n.ssid = ssid_of(ap);
                n.ap_path = nm_object_get_path(NM_OBJECT(ap));
                n.strength = nm_access_point_get_strength(ap);
                n.security = wifi_security(nm_access_point_get_flags(ap),
                                           nm_access_point_get_wpa_flags(ap),
                                           nm_access_point_get_rsn_flags(ap));
                GPtrArray* matching = nm_access_point_filter_connections(ap, connections);
                n.saved = matching && matching->len > 0;
                if (matching)
                    g_ptr_array_unref(matching);
                n.active = ap == active && state == NM_DEVICE_STATE_ACTIVATED;
                n.connecting = ap == active && activating(state);
                aps.push_back(std::move(n));
            }
        }
        networks_ = merge_networks(std::move(aps));

        const WifiNetwork* now = active();
        const std::string current = now ? now->ssid : "";
        if (seen_ && !current.empty() && current != last_active_)
            for (auto& l : connected_listeners_)
                l(current);
        last_active_ = current;
        seen_ = running();
        for (auto& l : listeners_)
            l();
    }

    const char* Network::icon() const {
        if (wired_)
            return "fenriz-ethernet-symbolic";
        if (!enabled_)
            return "fenriz-wifi-off-symbolic";
        for (const WifiNetwork& n : networks_) {
            if (n.active)
                return signal_icon(n.strength);
            if (n.connecting)
                return "fenriz-wifi-sync-symbolic";
        }
        return "fenriz-wifi-strength-off-symbolic";
    }

    const WifiNetwork* Network::active() const {
        for (const WifiNetwork& n : networks_)
            if (n.active)
                return &n;
        return nullptr;
    }

    void Network::on_device_state(NMDevice*, guint new_state, guint, guint reason, gpointer data) {
        auto* self = static_cast<Network*>(data);
        self->schedule_refresh();
        if (self->pending_ssid_.empty())
            return;
        if (new_state == NM_DEVICE_STATE_ACTIVATED) {
            self->pending_ssid_.clear();
            self->pending_added_.clear();
        } else if (new_state == NM_DEVICE_STATE_FAILED) {
            const std::string ssid = self->pending_ssid_;
            const bool secrets = reason == NM_DEVICE_STATE_REASON_NO_SECRETS ||
                                 reason == NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT ||
                                 reason == NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT;
            // a connection we just made with a wrong password is not worth keeping
            if (!self->pending_added_.empty())
                if (NMRemoteConnection* c =
                        nm_client_get_connection_by_path(self->client_, self->pending_added_.c_str()))
                    nm_remote_connection_delete_async(c, nullptr, nullptr, nullptr);
            self->pending_ssid_.clear();
            self->pending_added_.clear();
            self->fail(secrets ? "Wrong password for " + ssid : "Couldn't connect to " + ssid, secrets ? ssid : "");
        }
    }

    void Network::fail(const std::string& message, const std::string& ssid) {
        g_message("network: %s", message.c_str());
        for (auto& l : error_listeners_)
            l(message, ssid);
    }

    void Network::set_enabled(bool on) {
        if (!running())
            return;
        nm_client_dbus_set_property(
            client_,
            NM_DBUS_PATH,
            NM_DBUS_INTERFACE,
            "WirelessEnabled",
            g_variant_new_boolean(on),
            -1,
            cancel_,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                const std::string e = finish_error(
                    [&](GError** err) { nm_client_dbus_set_property_finish(NM_CLIENT(source), res, err); });
                if (!e.empty())
                    static_cast<Network*>(data)->fail("Couldn't switch Wi-Fi: " + e, "");
            },
            this);
    }

    // NetworkManager rate-limits scans itself and says so with an error, which is not worth a word.
    void Network::scan() {
        if (wifi_ && enabled_)
            nm_device_wifi_request_scan_async(wifi_, cancel_, nullptr, nullptr);
    }

    NMRemoteConnection* Network::saved_connection(const WifiNetwork& n) const {
        NMAccessPoint* ap = wifi_ ? nm_device_wifi_get_access_point_by_path(wifi_, n.ap_path.c_str()) : nullptr;
        if (!ap)
            return nullptr;
        GPtrArray* matching = nm_access_point_filter_connections(ap, nm_client_get_connections(client_));
        NMRemoteConnection* c =
            matching && matching->len ? NM_REMOTE_CONNECTION(g_ptr_array_index(matching, 0)) : nullptr;
        if (matching)
            g_ptr_array_unref(matching);
        return c;
    }

    void Network::connect(const std::string& ssid, const std::string& password) {
        auto it =
            std::find_if(networks_.begin(), networks_.end(), [&](const WifiNetwork& n) { return n.ssid == ssid; });
        if (!wifi_ || it == networks_.end())
            return;
        const WifiNetwork& n = *it;
        pending_ssid_ = ssid;
        pending_added_.clear();

        const auto activated = +[](GObject* source, GAsyncResult* res, gpointer data) {
            GError* err = nullptr;
            NMActiveConnection* ac = nm_client_activate_connection_finish(NM_CLIENT(source), res, &err);
            if (ac)
                g_object_unref(ac);
            if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                auto* self = static_cast<Network*>(data);
                const std::string ssid = self->pending_ssid_;
                self->pending_ssid_.clear();
                self->fail("Couldn't connect to " + ssid + ": " + err->message, "");
            }
            g_clear_error(&err);
        };

        NMRemoteConnection* saved = n.saved ? saved_connection(n) : nullptr;
        if (saved && password.empty()) {
            nm_client_activate_connection_async(
                client_, NM_CONNECTION(saved), NM_DEVICE(wifi_), n.ap_path.c_str(), cancel_, activated, this);
            return;
        }
        if (saved) {
            // a new password for a network we know: keep its other settings, replace the secret
            NMSettingWirelessSecurity* sec = nm_connection_get_setting_wireless_security(NM_CONNECTION(saved));
            if (sec) {
                g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, password.c_str(), nullptr);
                nm_remote_connection_commit_changes_async(
                    saved,
                    TRUE,
                    cancel_,
                    +[](GObject* source, GAsyncResult* res, gpointer data) {
                        auto* self = static_cast<Network*>(data);
                        const std::string e = finish_error([&](GError** err) {
                            nm_remote_connection_commit_changes_finish(NM_REMOTE_CONNECTION(source), res, err);
                        });
                        if (!e.empty()) {
                            self->pending_ssid_.clear();
                            self->fail("Couldn't save the password: " + e, "");
                            return;
                        }
                        self->connect(self->pending_ssid_, "");
                    },
                    this);
                return;
            }
        }

        // a network we have never joined: NetworkManager fills in the rest from the access point
        NMConnection* partial = nullptr;
        if (n.security == WifiSecurity::Psk || n.security == WifiSecurity::Sae) {
            partial = nm_simple_connection_new();
            NMSetting* sec = nm_setting_wireless_security_new();
            g_object_set(sec,
                         NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                         n.security == WifiSecurity::Sae ? "sae" : "wpa-psk",
                         NM_SETTING_WIRELESS_SECURITY_PSK,
                         password.c_str(),
                         nullptr);
            nm_connection_add_setting(partial, sec);
        }
        nm_client_add_and_activate_connection_async(
            client_,
            partial,
            NM_DEVICE(wifi_),
            n.ap_path.c_str(),
            cancel_,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                GError* err = nullptr;
                NMActiveConnection* ac = nm_client_add_and_activate_connection_finish(NM_CLIENT(source), res, &err);
                if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    g_error_free(err);
                    return;
                }
                auto* self = static_cast<Network*>(data);
                if (ac) {
                    if (NMRemoteConnection* c = nm_active_connection_get_connection(ac))
                        self->pending_added_ = nm_object_get_path(NM_OBJECT(c));
                    g_object_unref(ac);
                } else {
                    const std::string ssid = self->pending_ssid_;
                    self->pending_ssid_.clear();
                    self->fail("Couldn't connect to " + ssid + ": " + (err ? err->message : "unknown error"), "");
                }
                g_clear_error(&err);
            },
            this);
        if (partial)
            g_object_unref(partial);
    }

    void Network::disconnect() {
        if (wifi_)
            nm_device_disconnect_async(NM_DEVICE(wifi_), cancel_, nullptr, nullptr);
    }

    void Network::forget(const std::string& ssid) {
        auto it =
            std::find_if(networks_.begin(), networks_.end(), [&](const WifiNetwork& n) { return n.ssid == ssid; });
        if (it == networks_.end())
            return;
        if (NMRemoteConnection* c = saved_connection(*it))
            nm_remote_connection_delete_async(c, cancel_, nullptr, nullptr);
    }

} // namespace fenriz::bar
