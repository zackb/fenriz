#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    struct BtDevice {
        std::string path;
        std::string name;      // Alias, which BlueZ fills from Name or the address
        std::string icon;      // freedesktop device icon (audio-headphones), "" when unknown
        bool has_name = false; // a real Name was advertised, not just an address
        bool paired = false;
        bool trusted = false;
        bool connected = false;
        int battery = -1;      // percent, -1 when not reported
        int rssi = G_MININT16; // signal, only while discovering
        bool busy = false;     // a connect/disconnect/pair we started is still running

        bool operator==(const BtDevice&) const = default;
    };

    // Paired devices first (connected before not, then by name), then the rest by signal strength.
    void sort_devices(std::vector<BtDevice>& devices);

    // Devices that became connected or disconnected between two snapshots, by path.
    struct BtChanges {
        std::vector<BtDevice> connected;
        std::vector<BtDevice> disconnected;
    };
    BtChanges bt_changes(const std::vector<BtDevice>& before, const std::vector<BtDevice>& after);

    // Symbolic icon name for a device, from BlueZ's Icon property.
    std::string bt_device_icon(const std::string& icon);

    // BlueZ over the system bus: the first adapter, its devices, and pairing through a no-input agent.
    class Bluetooth {
    public:
        Bluetooth() = default;
        ~Bluetooth();

        Bluetooth(const Bluetooth&) = delete;
        Bluetooth& operator=(const Bluetooth&) = delete;

        void start();
        void subscribe(std::function<void()> listener);
        // Called with a device that connected or disconnected since the last update.
        void on_connection(std::function<void(const BtDevice&, bool connected)> listener);
        // Called when an operation we started failed, with a user-facing reason.
        void on_error(std::function<void(const std::string&)> listener);

        bool available() const { return adapter_ != nullptr; }
        bool powered() const { return powered_; }
        bool discovering() const { return discovering_; }
        const std::vector<BtDevice>& devices() const { return devices_; }

        void set_powered(bool on);
        // Discovery runs while at least one caller wants it; each discover(true) needs a discover(false).
        void discover(bool on);
        void connect(const std::string& path);
        void disconnect(const std::string& path);
        // Pair, trust, then connect.
        void pair(const std::string& path);
        void forget(const std::string& path);

    private:
        struct Op;

        void refresh();
        void call(const std::string& path,
                  const char* iface,
                  const char* method,
                  GVariant* args,
                  int timeout_ms,
                  std::function<void(bool ok, const std::string& error)> done);
        void set_busy(const std::string& path, bool busy);
        void register_agent();

        static void on_manager(GObject* source, GAsyncResult* res, gpointer data);

        GCancellable* cancel_ = nullptr;
        GDBusObjectManager* manager_ = nullptr;
        GDBusProxy* adapter_ = nullptr;
        GDBusConnection* bus_ = nullptr;
        guint agent_id_ = 0;
        bool powered_ = false;
        bool discovering_ = false;
        int discover_wanted_ = 0;
        std::string pairing_; // device we asked to pair; the agent only says yes to it
        std::vector<std::string> busy_;
        std::vector<BtDevice> devices_;
        bool seen_ = false;
        std::vector<std::function<void()>> listeners_;
        std::vector<std::function<void(const BtDevice&, bool)>> connection_listeners_;
        std::vector<std::function<void(const std::string&)>> error_listeners_;
    };

} // namespace fenriz::bar
