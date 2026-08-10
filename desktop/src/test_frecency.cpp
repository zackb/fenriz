#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "frecency.hpp"

using namespace fenriz::desktop;

namespace {

    constexpr int64_t DAY = 86400000;

    bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

    void test_recency_buckets() {
        assert(near(recency_weight(0), 1.0));
        assert(near(recency_weight(DAY - 1), 1.0));
        assert(near(recency_weight(DAY), 0.7)); // boundaries land in the older bucket
        assert(near(recency_weight(3 * DAY - 1), 0.7));
        assert(near(recency_weight(3 * DAY), 0.5));
        assert(near(recency_weight(7 * DAY - 1), 0.5));
        assert(near(recency_weight(7 * DAY), 0.25));
        assert(near(recency_weight(30 * DAY - 1), 0.25));
        assert(near(recency_weight(30 * DAY), 0.1));
        assert(near(recency_weight(3650 * DAY), 0.1));
    }

    void test_frecency_is_count_times_weight() {
        const int64_t now = 1000 * DAY;
        assert(near(frecency(5, now, now), 5.0));           // 5 launches, just now
        assert(near(frecency(5, now - 4 * DAY, now), 2.5)); // same count, a week-ish old
        assert(near(frecency(1, now, now), 1.0));
        assert(near(frecency(0, now, now), 0.0));  // never launched
        assert(near(frecency(-3, now, now), 0.0)); // corrupt store must not score
    }

    // Daily use over a month must outrank a single launch from yesterday.
    void test_frequent_beats_merely_recent() {
        const int64_t now = 1000 * DAY;
        assert(frecency(40, now - 2 * DAY, now) > frecency(1, now, now));
    }

    // A clock step backwards would otherwise read as a launch in the future.
    void test_future_timestamp_is_not_maximally_recent() {
        const int64_t now = 1000 * DAY;
        assert(near(frecency(3, now + 5 * DAY, now), 3.0)); // treated as age 0, not negative
    }

    void test_store_records_and_scores() {
        const int64_t now = 1000 * DAY;
        UsageStore s;
        s.seed("a.desktop", {10, now});
        s.seed("b.desktop", {10, now - 10 * DAY});
        assert(s.score_of("a.desktop", now) > s.score_of("b.desktop", now));
        assert(near(s.score_of("never.desktop", now), 0.0)); // unknown id
    }

    // Round-trips through the on-disk key file, into a scratch XDG_STATE_HOME.
    void test_store_persists() {
        char tmpl[] = "/tmp/fenriz-desktop-testXXXXXX";
        const char* dir = mkdtemp(tmpl);
        assert(dir != nullptr);
        setenv("XDG_STATE_HOME", dir, 1);

        const int64_t now = 1000 * DAY;
        {
            UsageStore w;
            w.load(); // no file yet
            assert(w.uses().empty());
            w.record("kitty.desktop", now);
            w.record("kitty.desktop", now);
            w.record("firefox.desktop", now);
        }
        UsageStore r;
        r.load();
        assert(r.uses().size() == 2);
        assert(r.uses().at("kitty.desktop").count == 2);
        assert(r.uses().at("kitty.desktop").last == now);
        assert(r.uses().at("firefox.desktop").count == 1);
        assert(r.score_of("kitty.desktop", now) > r.score_of("firefox.desktop", now));

        std::string path = UsageStore::path();
        unlink(path.c_str());
    }

    void test_empty_id_is_ignored() {
        UsageStore s;
        s.record("", 1);
        assert(s.uses().empty());
    }

    // State lives beside fenriz's own, not in a directory of its own.
    void test_state_path() {
        setenv("XDG_STATE_HOME", "/tmp/state-under-test", 1);
        assert(UsageStore::path() == "/tmp/state-under-test/fenriz/launcher.usage");

        // Falling back to ~/.local/state keeps the same shape.
        unsetenv("XDG_STATE_HOME");
        const std::string fallback = UsageStore::path();
        assert(fallback.find("/.local/state/fenriz/launcher.usage") != std::string::npos);
    }

} // namespace

int main() {
    test_recency_buckets();
    test_frecency_is_count_times_weight();
    test_frequent_beats_merely_recent();
    test_future_timestamp_is_not_maximally_recent();
    test_store_records_and_scores();
    test_store_persists();
    test_empty_id_is_ignored();
    test_state_path();
    return 0;
}
