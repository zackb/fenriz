#include <cassert>

#include "sysstat.hpp"
#include "upower.hpp"

using namespace fenriz::bar;

namespace {

    void test_cpu() {
        const auto a = parse_cpu("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 1 2 3 4 5 6 7 8 9 10\n");
        assert(a.total == 1000);
        assert(a.idle == 850);
        const auto b = parse_cpu("cpu  200 0 100 850 50 0 0 0 0 0\n");
        // 200 more total, 50 more idle: 75% busy
        assert(cpu_percent(a, b) == 75);
        assert(cpu_percent(b, a) == 0); // counters never run backwards, but a bad read must not underflow
        assert(parse_cpu("garbage").total == 0);
    }

    void test_memory() {
        assert(parse_memory_percent("MemTotal:       1000 kB\nMemFree:  10 kB\nMemAvailable:   250 kB\n") == 75);
        assert(parse_memory_percent("") == -1);
        assert(parse_memory_percent("MemTotal: 100 kB\nMemAvailable: 900 kB\n") == -1);
    }

    void test_duration() {
        assert(format_duration(0).empty());
        assert(format_duration(-5).empty());
        assert(format_duration(59) == "1 min");
        assert(format_duration(45 * 60) == "45 min");
        assert(format_duration(60 * 60) == "1 h");
        assert(format_duration(2 * 3600 + 13 * 60) == "2 h 13 min");
    }

    void test_charge_states() {
        assert(charge_from_upower(1) == Charge::Charging);
        assert(charge_from_upower(5) == Charge::Charging);
        assert(charge_from_upower(2) == Charge::Discharging);
        assert(charge_from_upower(6) == Charge::Discharging);
        assert(charge_from_upower(4) == Charge::Full);
        assert(charge_from_upower(0) == Charge::Unknown);
    }

    Battery reading(Charge charge, double percent) {
        Battery b;
        b.present = true;
        b.charge = charge;
        b.percent = percent;
        return b;
    }

    void test_battery_changes() {
        // the first reading only reports an already-low battery
        assert(!battery_change({}, reading(Charge::Charging, 50), true).plugged);
        assert(battery_change({}, reading(Charge::Discharging, 8), true).low);

        auto c = battery_change(reading(Charge::Discharging, 50), reading(Charge::Charging, 50), false);
        assert(c.plugged && !c.unplugged);
        c = battery_change(reading(Charge::Full, 100), reading(Charge::Discharging, 100), false);
        assert(c.unplugged && !c.plugged);
        // UPower settling after start is not an unplug
        assert(!battery_change(reading(Charge::Unknown, 80), reading(Charge::Discharging, 80), false).unplugged);
        // Full -> Charging (topping up) is not a plug-in
        assert(!battery_change(reading(Charge::Full, 100), reading(Charge::Charging, 99), false).plugged);

        // low fires once on the crossing, not on every reading below it
        assert(battery_change(reading(Charge::Discharging, 11), reading(Charge::Discharging, 10), false).low);
        assert(!battery_change(reading(Charge::Discharging, 10), reading(Charge::Discharging, 9), false).low);
        // plugging in recovers
        c = battery_change(reading(Charge::Discharging, 9), reading(Charge::Charging, 9), false);
        assert(c.recovered && c.plugged && !c.low);
        // no battery, nothing
        assert(!battery_change({}, {}, false).plugged);
    }

} // namespace

int main() {
    test_cpu();
    test_memory();
    test_duration();
    test_charge_states();
    test_battery_changes();
    return 0;
}
