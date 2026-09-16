#include <cassert>

#include "network.hpp"

using namespace fenriz::bar;

namespace {

    void test_security() {
        assert(wifi_security(0, 0, 0) == WifiSecurity::Open);
        assert(wifi_security(NM_802_11_AP_FLAGS_PRIVACY, 0, NM_802_11_AP_SEC_KEY_MGMT_PSK) == WifiSecurity::Psk);
        assert(wifi_security(NM_802_11_AP_FLAGS_PRIVACY, NM_802_11_AP_SEC_KEY_MGMT_PSK, 0) == WifiSecurity::Psk);
        // WPA3 transition mode advertises both; PSK still joins it
        assert(wifi_security(1, 0, NM_802_11_AP_SEC_KEY_MGMT_PSK | NM_802_11_AP_SEC_KEY_MGMT_SAE) == WifiSecurity::Psk);
        assert(wifi_security(1, 0, NM_802_11_AP_SEC_KEY_MGMT_SAE) == WifiSecurity::Sae);
        assert(wifi_security(1, 0, NM_802_11_AP_SEC_KEY_MGMT_802_1X) == WifiSecurity::Unsupported);
        assert(wifi_security(0, 0, NM_802_11_AP_SEC_KEY_MGMT_OWE) == WifiSecurity::Open);
        // privacy with no WPA/RSN is WEP
        assert(wifi_security(NM_802_11_AP_FLAGS_PRIVACY, 0, 0) == WifiSecurity::Unsupported);
    }

    WifiNetwork ap(const char* ssid, int strength, bool saved = false, bool active = false) {
        WifiNetwork n;
        n.ssid = ssid;
        n.ap_path = std::string("/ap/") + ssid + std::to_string(strength);
        n.strength = strength;
        n.saved = saved;
        n.active = active;
        return n;
    }

    void test_merge() {
        auto list = merge_networks({
            ap("Cafe", 40),
            ap("", 99), // hidden
            ap("Home", 60, true),
            ap("Home", 90, true), // the stronger mesh node of the same network
            ap("Office", 85),
            ap("Home5G", 30, true, true),
            ap("Apt 2", 88),
        });
        assert(list.size() == 5);
        assert(list[0].ssid == "Home5G"); // active first
        assert(list[1].ssid == "Home" && list[1].strength == 90 && list[1].ap_path == "/ap/Home90");
        // then by bars (Apt 2 and Office are both 4 bars, so by name), then Cafe at 2 bars
        assert(list[2].ssid == "Apt 2");
        assert(list[3].ssid == "Office");
        assert(list[4].ssid == "Cafe");

        // the access point in use stays the chosen one even when another node of its network is stronger
        list = merge_networks({ap("Home", 50, true, true), ap("Home", 95, true)});
        assert(list.size() == 1 && list[0].active && list[0].strength == 50);
        list = merge_networks({ap("Home", 95, true), ap("Home", 50, true, true)});
        assert(list.size() == 1 && list[0].active && list[0].strength == 50);
    }

    void test_secrets_failure() {
        assert(wifi_secrets_failure(NM_DEVICE_STATE_REASON_NO_SECRETS));
        assert(wifi_secrets_failure(NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT));
        assert(wifi_secrets_failure(NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT));
        assert(!wifi_secrets_failure(NM_DEVICE_STATE_REASON_SSID_NOT_FOUND));
        assert(!wifi_secrets_failure(NM_DEVICE_STATE_REASON_NONE));
    }

    void test_bars() {
        assert(signal_bars(0) == 0);
        assert(signal_bars(29) == 1);
        assert(signal_bars(30) == 2);
        assert(signal_bars(79) == 3);
        assert(signal_bars(100) == 4);
    }

} // namespace

int main() {
    test_security();
    test_merge();
    test_bars();
    test_secrets_failure();
    return 0;
}
