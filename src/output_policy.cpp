// The pure half of output management: which output each workspace belongs on, what counts as a
// lid-controlled panel, and what scale a screen wants. No wlroots, no compositor state,
// output.cpp maps the result back onto real outputs. Split out so test_output.cpp can link it
// without a display.

#include "output.hpp"

#include <algorithm>
#include <cmath>

namespace fenriz::output {

    bool is_internal(const std::string& name) {
        // wlroots exposes no "built-in panel" bit, so go by connector name like sway/Hyprland.
        return name.rfind("eDP-", 0) == 0 || name.rfind("LVDS-", 0) == 0 || name.rfind("DSI-", 0) == 0;
    }

    float guess_scale(int phys_w_mm, int phys_h_mm, int px_w, int px_h) {
        if (phys_w_mm <= 0 || phys_h_mm <= 0 || px_w <= 0 || px_h <= 0)
            return 1.0f;

        // EDIDs lie (projector, TV)
        const double diag_mm = std::hypot((double)phys_w_mm, (double)phys_h_mm);
        if (diag_mm < 100.0)
            return 1.0f;

        if (px_h < 1200)
            return 1.0f;

        const double dpi = std::hypot((double)px_w, (double)px_h) / (diag_mm / 25.4);
        if (dpi >= 192.0)
            return 2.0f;
        if (dpi >= 144.0)
            return 1.5f;
        return 1.0f;
    }

    double zoom_viewport_origin(double c, double z) { return z > 1.0 ? c * (1.0 - 1.0 / z) : 0.0; }

    void assign_workspaces(const std::string home[WS_MAX],
                           const bool needed[WS_MAX],
                           const std::vector<std::string>& live,
                           std::string current[WS_MAX],
                           std::string origin[WS_MAX],
                           int count) {
        auto is_live = [&](const std::string& n) {
            return !n.empty() && std::find(live.begin(), live.end(), n) != live.end();
        };

        for (int i = 0; i < count; i++) {
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
                // Unassigned. Deliberately NOT parked on live.front(): that would pin every
                // workspace to the first screen and leave a second monitor nothing to show.
                current[i].clear();
            }
        }
    }

    void assign_active(std::vector<OutSlot>& outs, WsSlot ws[WS_MAX], int focused_ws, int count) {
        // 1. Drop the shown workspace of any output that no longer holds it (or is off).
        for (OutSlot& o : outs)
            if (!o.enabled || (o.active_ws >= 0 && ws[o.active_ws].output != o.name))
                o.active_ws = -1;

        // 2. Your work follows you. If the focused window's workspace was just evacuated,
        // show it on the screen it landed on. Leaving the external on whatever empty
        // workspace it happened to display would strand the session for no reason.
        if (focused_ws >= 0 && focused_ws < count && !ws[focused_ws].output.empty())
            for (OutSlot& o : outs)
                if (o.name == ws[focused_ws].output)
                    o.active_ws = focused_ws;

        // 3. Two outputs must never show the same workspace. Earlier in `outs` wins, which
        // is the order the outputs appeared, stable across an unrelated hotplug.
        for (size_t i = 0; i < outs.size(); i++)
            for (size_t j = 0; j < outs.size(); j++)
                if (i != j && outs[i].active_ws >= 0 && outs[i].active_ws == outs[j].active_ws)
                    outs[j].active_ws = -1;

        // 4. Every enabled output shows exactly one workspace.
        for (OutSlot& o : outs) {
            if (!o.enabled || o.active_ws >= 0)
                continue;

            // Best workspace already living here: a configured home beats one with windows
            // beats an empty one (rank 0..3, lowest wins).
            int best = -1, best_rank = 99;
            for (int i = 0; i < count; i++) {
                if (ws[i].output != o.name)
                    continue;
                const bool homed = ws[i].home == o.name;
                const int rank = homed ? (ws[i].has_windows ? 0 : 1) : (ws[i].has_windows ? 2 : 3);
                if (rank < best_rank) {
                    best_rank = rank;
                    best = i;
                }
            }
            if (best < 0) {
                // Nothing lives here yet: claim the lowest-numbered unassigned workspace,
                // preferring one configured for this output. Never steal one that is spoken
                // for, a workspace configured for ANOTHER output stays free for it.
                for (int i = 0; i < count && best < 0; i++)
                    if (ws[i].output.empty() && ws[i].home == o.name)
                        best = i;
                for (int i = 0; i < count && best < 0; i++)
                    if (ws[i].output.empty() && ws[i].home.empty())
                        best = i;
                if (best >= 0)
                    ws[best].output = o.name;
            }
            o.active_ws = best; // -1 only if all `count` are spoken for elsewhere
        }
    }

} // namespace fenriz::output
