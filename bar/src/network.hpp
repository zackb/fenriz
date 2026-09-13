#pragma once

#include <NetworkManager.h>

#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    enum class WifiSecurity {
        Open,
        Psk,         // WPA/WPA2 personal, and WPA3 transition networks
        Sae,         // WPA3 personal only
        Unsupported, // enterprise (802.1X) and WEP: set up with nm-connection-editor
    };

    // From an access point's Flags, WpaFlags and RsnFlags.
    WifiSecurity wifi_security(guint32 flags, guint32 wpa, guint32 rsn);

    struct WifiNetwork {
        std::string ssid;
        std::string ap_path; // the strongest access point for this SSID
        int strength = 0;    // 0..100
        WifiSecurity security = WifiSecurity::Open;
        bool saved = false;
        bool active = false;
        bool connecting = false;

        bool operator==(const WifiNetwork&) const = default;
    };

    // One entry per SSID (the strongest access point wins; hidden networks with no SSID are dropped), ordered active,
    // saved, then by signal bars and name so small strength changes do not reshuffle the list.
    std::vector<WifiNetwork> merge_networks(std::vector<WifiNetwork> aps);

    // 0..4 bars.
    int signal_bars(int strength);
    const char* signal_icon(int strength);

    // NetworkManager through libnm: the first Wi-Fi device, its networks, saved connections, and whether a wire is up.
    class Network {
    public:
        Network() = default;
        ~Network();

        Network(const Network&) = delete;
        Network& operator=(const Network&) = delete;

        void start();
        void subscribe(std::function<void()> listener);
        void on_connected(std::function<void(const std::string& ssid)> listener);
        // An error for the user; `ssid` is set when the network needs its password (again).
        void on_error(std::function<void(const std::string& message, const std::string& ssid)> listener);

        bool running() const { return client_ && nm_client_get_nm_running(client_); }
        bool has_wifi() const { return wifi_ != nullptr; }
        bool wifi_enabled() const { return enabled_; }
        bool wired() const { return wired_; }
        const std::vector<WifiNetwork>& networks() const { return networks_; }
        // The connected network, or null.
        const WifiNetwork* active() const;
        // What the bar shows: wired, off, the connected network's bars, connecting, or offline.
        const char* icon() const;

        void set_enabled(bool on);
        void scan();
        // A saved network needs no password; a new secured one does. A password for a saved network replaces its old
        // one.
        void connect(const std::string& ssid, const std::string& password);
        void disconnect();
        void forget(const std::string& ssid);

    private:
        void schedule_refresh();
        void refresh();
        void watch_devices();
        NMRemoteConnection* saved_connection(const WifiNetwork& n) const;
        void fail(const std::string& message, const std::string& ssid);

        static void on_client(GObject* source, GAsyncResult* res, gpointer data);
        static void on_device_state(NMDevice* device, guint new_state, guint old_state, guint reason, gpointer data);

        GCancellable* cancel_ = nullptr;
        NMClient* client_ = nullptr;
        NMDeviceWifi* wifi_ = nullptr;
        std::vector<NMDevice*> watched_;
        guint refresh_id_ = 0;
        bool enabled_ = false;
        bool wired_ = false;
        std::vector<WifiNetwork> networks_;
        std::string pending_ssid_;  // a connect we started and have not seen finish
        std::string pending_added_; // path of a connection we created for it, deleted if the attempt fails
        std::string last_active_;
        bool seen_ = false;
        std::vector<std::function<void()>> listeners_;
        std::vector<std::function<void(const std::string&)>> connected_listeners_;
        std::vector<std::function<void(const std::string&, const std::string&)>> error_listeners_;
    };

} // namespace fenriz::bar
