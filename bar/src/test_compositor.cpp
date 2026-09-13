#include <cassert>

#include "compositor.hpp"

using fenriz::bar::parse_state;

namespace {

    // The example line from docs/IPC.md, with a second screen.
    constexpr const char* TWO_SCREENS =
        R"({"outputs":[{"name":"eDP-1","active":1,"focused":false,"x":0,"y":0,"width":2560,"height":1600,"scale":2.0,"internal":true},)"
        R"({"name":"DP-1","active":4,"focused":true,"x":2560,"y":0,"width":3840,"height":2160,"scale":1.0,"internal":false}],)"
        R"("lid":"open","cursor":{"x":100,"y":200},)"
        R"("workspaces":{"active":4,"occupied":[1,2,4],"urgent":[2]},)"
        R"("windows":[{"appId":"foot","title":"~","icon":"","tag":"","workspace":4,"floating":false,"fullscreen":false,"focused":true,"urgent":false}],)"
        R"("activeWindow":{"appId":"foot","title":"~ \"quoted\"","icon":"utilities-terminal","tag":""}})";

    void test_parses_per_output_state() {
        auto s = parse_state(TWO_SCREENS);
        assert(s);
        assert(s->outputs.size() == 2);
        assert(s->active_on("eDP-1") == 1);
        assert(s->active_on("DP-1") == 4);
        assert(s->active_on("HDMI-A-1") == 0);
        assert(s->focused_output() == "DP-1");
        assert((s->occupied == std::vector<int>{1, 2, 4}));
        assert((s->urgent == std::vector<int>{2}));
        assert(s->has_window);
        assert(s->title == "~ \"quoted\"");
        assert(s->app_id == "foot");
        assert(s->icon == "utilities-terminal");
    }

    void test_null_active_window() {
        auto s = parse_state(
            R"({"outputs":[],"workspaces":{"active":1,"occupied":[],"urgent":[]},"windows":[],"activeWindow":null})");
        assert(s);
        assert(!s->has_window);
        assert(s->title.empty());
        assert(s->focused_output().empty());
    }

    // The event feed and garbage share nothing with a state line and must not blank the bar.
    void test_rejects_non_state() {
        assert(!parse_state(R"({"event":"bell","appId":"kitty"})"));
        assert(!parse_state("not json"));
        assert(!parse_state("[1,2,3]"));
        assert(!parse_state(""));
    }

    void test_equal_snapshots_compare_equal() { assert(*parse_state(TWO_SCREENS) == *parse_state(TWO_SCREENS)); }

} // namespace

int main() {
    test_parses_per_output_state();
    test_null_active_window();
    test_rejects_non_state();
    test_equal_snapshots_compare_equal();
    return 0;
}
