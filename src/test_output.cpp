// Self-check for the workspace-assignment policy — the clamshell guarantee in pure form.
// Deliberately free of wlroots: builds and runs without a display.

#include "output.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace fenriz;
using namespace fenriz::output;

namespace {

    // The four parallel arrays the policy works on, plus enough sugar to keep the cases legible.
    struct WS {
        std::string home[WS_COUNT];
        bool needed[WS_COUNT] = {};
        std::string current[WS_COUNT];
        std::string origin[WS_COUNT];

        // Workspace n (0-indexed) has windows and currently lives on `output`.
        void live_on(int n, const std::string& output) {
            current[n] = output;
            needed[n] = true;
        }
        void run(const std::vector<std::string>& live) { assign_workspaces(home, needed, live, current, origin); }
    };

    const std::vector<std::string> BOTH = {"eDP-1", "DP-1"};
    const std::vector<std::string> EXTERNAL_ONLY = {"DP-1"};
    const std::vector<std::string> PANEL_ONLY = {"eDP-1"};

} // namespace

int main() {
    // ---- The headline: a lid cycle round-trips with NO config at all. ----
    // ws1-2 have windows on the laptop panel, ws3 on the external. Nothing is homed.
    {
        WS s;
        s.live_on(0, "eDP-1");
        s.live_on(1, "eDP-1");
        s.live_on(2, "DP-1");

        s.run(EXTERNAL_ONLY); // lid closed while docked
        assert(s.current[0] == "DP-1" && s.origin[0] == "eDP-1");
        assert(s.current[1] == "DP-1" && s.origin[1] == "eDP-1");
        assert(s.current[2] == "DP-1" && s.origin[2].empty()); // was already there; never moved

        s.run(BOTH);                                            // lid opened
        assert(s.current[0] == "eDP-1" && s.origin[0].empty()); // came home, record cleared
        assert(s.current[1] == "eDP-1" && s.origin[1].empty());
        assert(s.current[2] == "DP-1");
    }

    // Repeated lid cycles must not drift — several output events fire per suspend/resume.
    {
        WS s;
        s.live_on(0, "eDP-1");
        s.live_on(2, "DP-1");
        for (int i = 0; i < 5; i++) {
            s.run(EXTERNAL_ONLY);
            assert(s.current[0] == "DP-1");
            s.run(BOTH);
            assert(s.current[0] == "eDP-1");
            assert(s.current[2] == "DP-1");
        }
    }

    // Idempotence: re-running on a settled layout changes nothing.
    {
        WS s;
        s.live_on(0, "eDP-1");
        s.live_on(2, "DP-1");
        s.run(BOTH);
        for (int i = 0; i < 5; i++)
            s.run(BOTH);
        assert(s.current[0] == "eDP-1" && s.current[2] == "DP-1");
        assert(s.origin[0].empty() && s.origin[2].empty());
    }

    // An explicit config home beats the evacuation record: `workspace = 1, DP-1` means ws1
    // belongs on DP-1 even though it was last evacuated off the panel.
    {
        WS s;
        s.home[0] = "DP-1";
        s.live_on(0, "eDP-1");
        s.run(EXTERNAL_ONLY);
        assert(s.current[0] == "DP-1");
        s.run(BOTH);
        assert(s.current[0] == "DP-1"); // home wins; does NOT go back to eDP-1
    }

    // ---- Idle workspaces stay unassigned, so a new monitor has something to claim. ----
    // This is the bug that made a second screen render nothing: parking all 10 on the first
    // output left none free.
    {
        WS s;
        s.live_on(0, "eDP-1"); // only ws1 is in use
        s.run(BOTH);
        assert(s.current[0] == "eDP-1");
        for (int i = 1; i < WS_COUNT; i++)
            assert(s.current[i].empty()); // ws2-10 free for any output to claim
    }

    // An idle workspace that gets windows while its screen is gone doesn't get stranded.
    {
        WS s;
        s.run(BOTH);
        assert(s.current[3].empty());
        s.live_on(3, "eDP-1"); // a window opens on ws4, on the panel
        s.run(EXTERNAL_ONLY);  // lid shuts
        assert(s.current[3] == "DP-1" && s.origin[3] == "eDP-1");
        s.run(BOTH);
        assert(s.current[3] == "eDP-1"); // and back
    }

    // Undocked: only the panel is live, so a lid event has nothing to evacuate to. logind
    // suspends here; fenriz must not shuffle anything meanwhile.
    {
        WS s;
        s.live_on(0, "eDP-1");
        s.live_on(1, "eDP-1");
        s.run(PANEL_ONLY);
        assert(s.current[0] == "eDP-1" && s.current[1] == "eDP-1");
        assert(s.origin[0].empty());
    }

    // Homed to a screen that has never appeared: falls back, keeps its home pending, and is
    // pulled over the moment that screen shows up.
    {
        WS s;
        s.home[0] = "HDMI-A-1";
        s.needed[0] = true;
        s.run(EXTERNAL_ONLY);
        assert(s.current[0] == "DP-1");
        s.run({"DP-1", "HDMI-A-1"});
        assert(s.current[0] == "HDMI-A-1");
    }

    // Every screen gone (suspend with the lid shut): nothing points at a dead output, and the
    // evacuation record survives so things land correctly when a screen returns.
    {
        WS s;
        s.live_on(0, "eDP-1");
        s.live_on(2, "DP-1");
        s.run({});
        for (int i = 0; i < WS_COUNT; i++)
            assert(s.current[i].empty());
        s.run(BOTH);
        assert(s.current[0] == "eDP-1"); // origin honored
        assert(s.current[2] == "DP-1");
    }

    // Unplugging the external moves its workspaces to the panel and back on replug — the same
    // machinery as the lid, with no lid involved.
    {
        WS s;
        s.live_on(2, "DP-1");
        s.run(PANEL_ONLY);
        assert(s.current[2] == "eDP-1" && s.origin[2] == "DP-1");
        s.run(BOTH);
        assert(s.current[2] == "DP-1");
    }

    // The internal-panel rule the lid policy keys off.
    assert(is_internal("eDP-1"));
    assert(is_internal("LVDS-1"));
    assert(is_internal("DSI-1"));
    assert(!is_internal("DP-1"));
    assert(!is_internal("HDMI-A-1"));
    assert(!is_internal(""));

    printf("test_output: ok\n");
    return 0;
}
