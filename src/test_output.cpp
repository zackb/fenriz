// Self-check for the workspace-assignment policy — the clamshell guarantee in pure form.
// Deliberately free of wlroots: builds and runs without a display.

#include "output.hpp"

#include <cassert>
#include <cmath>
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

    // The same shape for the other half of the policy: which workspace each output shows.
    struct Active {
        std::vector<OutSlot> outs;
        WsSlot ws[WS_COUNT];

        void output(const std::string& name, bool enabled = true, int active_ws = -1) {
            outs.push_back({name, enabled, active_ws});
        }
        // Workspace n lives on `on` and has windows.
        void live_on(int n, const std::string& on) {
            ws[n].output = on;
            ws[n].has_windows = true;
        }
        void run(int focused_ws = -1) { assign_active(outs, ws, focused_ws); }

        int shown(const std::string& name) const {
            for (const OutSlot& o : outs)
                if (o.name == name)
                    return o.active_ws;
            return -2; // no such output; distinct from -1 = "shows nothing"
        }
    };

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

    // ---- assign_active: which workspace each output SHOWS ----

    // The headline, and the bug that shipped: plug in a second monitor and it must CLAIM a
    // free workspace. Before the claim step it rendered nothing, and no keybind could fix it
    // because assign_workspaces had already handed every workspace to the first screen.
    {
        Active a;
        a.output("eDP-1", true, 0);
        a.output("DP-1"); // just appeared, shows nothing
        a.live_on(0, "eDP-1");
        a.run();
        assert(a.shown("eDP-1") == 0);
        assert(a.shown("DP-1") >= 0); // claimed something
        assert(a.shown("DP-1") != a.shown("eDP-1"));
        assert(a.ws[a.shown("DP-1")].output == "DP-1"); // and the claim was recorded
    }

    // Two outputs never show the same workspace. Whichever comes first in output order keeps
    // it; the other is pushed onto a free one rather than mirroring.
    {
        Active a;
        a.output("eDP-1", true, 3);
        a.output("DP-1", true, 3); // both think they're showing ws4
        a.ws[3].output = "eDP-1";
        a.run();
        assert(a.shown("eDP-1") == 3);
        assert(a.shown("DP-1") != 3);
        assert(a.shown("DP-1") >= 0); // and it still got something to show
    }

    // An output stops showing a workspace that has moved off it (the evacuation case), and a
    // disabled output shows nothing at all.
    {
        Active a;
        a.output("eDP-1", false, 0); // lid shut
        a.output("DP-1", true, 2);
        a.live_on(0, "DP-1"); // ws1 was evacuated to the external
        a.live_on(2, "DP-1");
        a.run();
        assert(a.shown("eDP-1") == -1); // off: shows nothing, claims nothing
        assert(a.shown("DP-1") == 2);
    }

    // Your work follows you: the focused workspace is shown on whichever output it now lives
    // on. This is the clamshell payoff — dock, shut the lid, keep working.
    {
        Active a;
        a.output("eDP-1", false, -1);
        a.output("DP-1", true, 5); // external happens to show an empty ws6
        a.live_on(0, "DP-1");      // the focused workspace was evacuated here
        a.run(/*focused_ws=*/0);
        assert(a.shown("DP-1") == 0);
    }

    // Picking among workspaces already living here: a configured home beats one with
    // windows, which beats an empty one.
    {
        Active a;
        a.output("DP-1");
        a.ws[7].output = "DP-1"; // empty, not homed        -> rank 3
        a.live_on(6, "DP-1");    // windows, not homed      -> rank 2
        a.ws[4].output = "DP-1";
        a.ws[4].home = "DP-1"; // homed, empty            -> rank 1
        a.run();
        assert(a.shown("DP-1") == 4);
    }

    // A claim prefers a workspace configured for this output, and never steals one
    // configured for a different screen.
    {
        Active a;
        a.output("DP-1");
        a.ws[0].home = "eDP-1"; // spoken for by a screen that isn't here
        a.ws[3].home = "DP-1";  // ours
        a.run();
        assert(a.shown("DP-1") == 3);
        assert(a.ws[0].output.empty()); // left free for eDP-1 to claim later
    }

    // Idempotence: re-running on a settled layout must not shuffle screens.
    {
        Active a;
        a.output("eDP-1");
        a.output("DP-1");
        a.live_on(0, "eDP-1");
        a.run();
        const int e = a.shown("eDP-1"), d = a.shown("DP-1");
        for (int i = 0; i < 5; i++)
            a.run();
        assert(a.shown("eDP-1") == e && a.shown("DP-1") == d);
    }

    // Every workspace spoken for elsewhere: -1 rather than stealing or crashing.
    {
        Active a;
        a.output("eDP-1", true, 0);
        a.output("DP-1");
        for (int i = 0; i < WS_COUNT; i++)
            a.live_on(i, "eDP-1");
        a.run();
        assert(a.shown("DP-1") == -1);
    }

    // The internal-panel rule the lid policy keys off.
    assert(is_internal("eDP-1"));
    assert(is_internal("LVDS-1"));
    assert(is_internal("DSI-1"));
    assert(!is_internal("DP-1"));
    assert(!is_internal("HDMI-A-1"));
    assert(!is_internal(""));

    assert(guess_scale(290, 190, 2880, 1920) == 2.0f);  // 13.5" laptop panel, 253 dpi
    assert(guess_scale(1190, 340, 3840, 1080) == 1.0f); // 49" 32:9 ultrawide, 82 dpi
    assert(guess_scale(600, 340, 3840, 2160) == 1.5f);  // 27" 4K, 163 dpi
    assert(guess_scale(600, 340, 2560, 1440) == 1.0f);  // 27" 1440p, 109 dpi
    assert(guess_scale(1600, 900, 3840, 2160) == 1.0f); // 72" TV, 61 dpi

    assert(guess_scale(0, 0, 2880, 1920) == 1.0f);
    assert(guess_scale(160, 90, 1920, 1080) == 1.0f); // implausibly small "monitor"
    assert(guess_scale(-1, -1, 2880, 1920) == 1.0f);
    assert(guess_scale(290, 190, 0, 0) == 1.0f); // no mode yet
    assert(guess_scale(100, 60, 1920, 1080) == 1.0f);

    {
        const double W = 1920;
        for (double z : {1.0, 1.01, 1.5, 2.0, 3.0, 10.0}) {
            const double vw = W / z;
            for (double c : {0.0, 1.0, 640.0, 960.0, 1279.0, W}) {
                const double v = zoom_viewport_origin(c, z);
                assert(std::abs((c - v) / vw * W - c) < 1e-9); // pointer's point is a fixed point
                assert(v >= 0.0 && v <= W - vw + 1e-9);        // in range without clamping
            }
        }
        assert(zoom_viewport_origin(1234.0, 1.0) == 0.0);
        assert(zoom_viewport_origin(0.0, 4.0) == 0.0);
        assert(std::abs(zoom_viewport_origin(W, 4.0) - (W - W / 4)) < 1e-9);
    }

    printf("test_output: ok\n");
    return 0;
}
