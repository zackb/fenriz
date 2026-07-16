// The pure half of output management: which output each workspace belongs on, and what counts
// as a lid-controlled panel. No wlroots, no compositor state, output.cpp maps the result back
// onto real outputs. Split out so test_output.cpp can link it without a display.

#include "output.hpp"

#include <algorithm>

namespace fenriz::output {

    bool is_internal(const std::string& name) {
        // wlroots exposes no "built-in panel" bit, so go by connector name like sway/Hyprland.
        return name.rfind("eDP-", 0) == 0 || name.rfind("LVDS-", 0) == 0 || name.rfind("DSI-", 0) == 0;
    }

    void assign_workspaces(const std::string home[WS_COUNT],
                           const bool needed[WS_COUNT],
                           const std::vector<std::string>& live,
                           std::string current[WS_COUNT],
                           std::string origin[WS_COUNT]) {
        auto is_live = [&](const std::string& n) {
            return !n.empty() && std::find(live.begin(), live.end(), n) != live.end();
        };

        for (int i = 0; i < WS_COUNT; i++) {
            if (is_live(home[i])) {
                current[i] = home[i]; // 1. configured home is back -> return; config always wins
                origin[i].clear();
            } else if (is_live(origin[i])) {
                // 2. the screen it was evacuated off is back -> undo the evacuation. This is
                // the whole no-config clamshell story: windows return to the laptop panel.
                current[i] = origin[i];
                origin[i].clear();
            } else if (is_live(current[i])) {
                // 3. still on a live output -> stay; don't churn on unrelated hotplugs.
            } else if (needed[i] && !live.empty()) {
                // 4. its screen died and it has something to show -> evacuate to a survivor,
                // remembering where it came from so rule 2 can bring it back.
                if (!current[i].empty())
                    origin[i] = current[i];
                current[i] = live.front();
            } else {
                // 5. nowhere to be: either idle, or every screen is gone (suspend with the lid
                // shut). Record where an in-use workspace was, or resuming would land it on
                // whichever screen comes back first instead of its own.
                if (needed[i] && !current[i].empty())
                    origin[i] = current[i];
                // Unassigned. Deliberately NOT parked on live.front(): that would pin all 10
                // workspaces to the first screen and leave a second monitor nothing to show.
                current[i].clear();
            }
        }
    }

} // namespace fenriz::output
