#include <cassert>

#include "bluetooth.hpp"

using namespace fenriz::bar;

namespace {

    BtDevice device(const char* path, const char* name, bool paired, bool connected, int rssi = G_MININT16) {
        BtDevice d;
        d.path = path;
        d.name = name;
        d.paired = paired;
        d.connected = connected;
        d.rssi = rssi;
        return d;
    }

    void test_sort() {
        std::vector<BtDevice> list = {
            device("/a", "Zeta phone", false, false, -80),
            device("/b", "Mouse", true, false),
            device("/c", "Speaker", false, false, -40),
            device("/d", "Earbuds", true, true),
            device("/e", "Controller", true, false),
        };
        sort_devices(list);
        // paired: connected first, then by name; unpaired: strongest signal first
        assert(list[0].path == "/d");
        assert(list[1].path == "/e");
        assert(list[2].path == "/b");
        assert(list[3].path == "/c");
        assert(list[4].path == "/a");
    }

    void test_changes() {
        const std::vector<BtDevice> before = {device("/a", "A", true, true), device("/b", "B", true, false)};
        std::vector<BtDevice> after = {device("/a", "A", true, false), device("/b", "B", true, true)};
        auto c = bt_changes(before, after);
        assert(c.connected.size() == 1 && c.connected[0].path == "/b");
        assert(c.disconnected.size() == 1 && c.disconnected[0].path == "/a");

        // nothing moved
        c = bt_changes(before, before);
        assert(c.connected.empty() && c.disconnected.empty());

        // the adapter powered off: every device vanished, and the connected one counts as disconnected
        c = bt_changes(before, {});
        assert(c.disconnected.size() == 1 && c.disconnected[0].path == "/a");

        // a new device that shows up already connected
        c = bt_changes({}, {device("/n", "New", true, true)});
        assert(c.connected.size() == 1);
    }

    void test_icon() {
        assert(bt_device_icon("audio-headphones") == "audio-headphones-symbolic");
        assert(bt_device_icon("") == "bluetooth-symbolic");
    }

} // namespace

int main() {
    test_sort();
    test_changes();
    test_icon();
    return 0;
}
