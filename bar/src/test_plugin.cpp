#include <cassert>

#include "plugin.hpp"

using fenriz::bar::apply_plugin_line;
using fenriz::bar::Plugin;
using fenriz::bar::plugin_event;
using fenriz::bar::PluginState;

namespace {

    void test_slots_replace_and_clear() {
        PluginState s;
        bool valid = false;
        unsigned changed = apply_plugin_line(
            s, R"({"chip": {"text": "14°", "icon": "weather-clear"}, "tile": {"title": "SEA"}})", &valid);
        assert(valid);
        assert(changed == (fenriz::bar::SLOT_CHIP | fenriz::bar::SLOT_TILE));
        assert(s.chip && s.chip->text == "14°" && s.chip->icon == "weather-clear");
        assert(s.tile && s.tile->title == "SEA");

        // absent slots are left alone, null clears
        changed = apply_plugin_line(s, R"({"tile": null})");
        assert(changed == fenriz::bar::SLOT_TILE);
        assert(!s.tile && s.chip);
    }

    // Plugins may re-send their whole state every poll; only real changes redraw or flash the pill.
    void test_unchanged_is_not_a_change() {
        PluginState s;
        const char* line = R"({"pill": {"text": "SEA 3–2"}})";
        assert(apply_plugin_line(s, line) == fenriz::bar::SLOT_PILL);
        assert(apply_plugin_line(s, line) == 0);
    }

    void test_page_blocks() {
        PluginState s;
        apply_plugin_line(s, R"({"page": {"title": "AL West", "blocks": [
            {"type": "row", "text": "Seattle", "trailing": 3, "action": "open"},
            {"type": "table", "columns": ["", "W"], "rows": [["SEA", 88], ["HOU", 80.5]], "highlight": 0},
            {"type": "list", "items": [{"text": "Standup", "subtitle": "9:30"}, "junk"]},
            {"type": "level", "value": 7},
            "junk"
        ]}})");
        assert(s.page && s.page->title == "AL West");
        assert(s.page->blocks.size() == 4);
        assert(s.page->blocks[0].trailing == "3" && s.page->blocks[0].action == "open");
        const auto& table = s.page->blocks[1];
        assert(table.columns.size() == 2 && table.rows.size() == 2);
        assert(table.rows[0][1] == "88" && table.rows[1][1] == "80.5" && table.highlight == 0);
        assert(s.page->blocks[2].items.size() == 1 && s.page->blocks[2].items[0].type == "row");
        assert(s.page->blocks[3].value == 1.0);
    }

    void test_garbage_is_skipped() {
        PluginState s;
        bool valid = true;
        assert(apply_plugin_line(s, "not json", &valid) == 0 && !valid);
        assert(apply_plugin_line(s, "[1, 2]", &valid) == 0 && !valid);
        assert(apply_plugin_line(s, R"({"chip": 5, "tile": "x"})", &valid) == 0 && valid);
        assert(!s.chip && !s.tile);
    }

    void test_events_are_json() {
        assert(plugin_event("open") == R"({"event":"open"})");
        assert(plugin_event("action", "say \"hi\"") == R"({"event":"action","id":"say \"hi\""})");
    }

    // Runs the main loop until `done` or a few seconds pass.
    void spin(const bool& done) {
        const gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        while (!done && g_get_monotonic_time() < deadline)
            g_main_context_iteration(nullptr, TRUE);
    }

    // A real process: answers an event on stdin, and its state is cleared when it exits.
    void test_process_round_trip() {
        Plugin p("echo",
                 R"(read line; case "$line" in *open*) echo '{"chip": {"text": "opened"}}';; esac; )"
                 R"(read line; echo '{"tile": {"title": "bye"}}')");
        bool opened = false, tiled = false, cleared = false;
        p.on_change([&](unsigned slots) {
            if ((slots & fenriz::bar::SLOT_CHIP) && p.state().chip)
                opened = p.state().chip->text == "opened";
            if ((slots & fenriz::bar::SLOT_TILE) && p.state().tile)
                tiled = true;
            if (tiled && !p.state().tile && !p.state().chip)
                cleared = true;
        });
        p.start();
        p.send(plugin_event("open"));
        spin(opened);
        assert(opened);
        p.send(plugin_event("close"));
        spin(cleared);
        assert(tiled && cleared);
    }

} // namespace

int main() {
    test_slots_replace_and_clear();
    test_unchanged_is_not_a_change();
    test_page_blocks();
    test_garbage_is_skipped();
    test_events_are_json();
    test_process_round_trip();
    return 0;
}
