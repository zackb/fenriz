#pragma once

#include <ctime>
#include <string>
#include <vector>
#include <wayland-server-core.h>

struct wlr_output;
struct wlr_scene_rect;

namespace fenriz {

    class Server;

    // Number of workspaces. Fixed set, shared across all outputs (each workspace lives on
    // exactly one output at a time). ponytail: 10 is plenty; dynamic workspaces if asked.
    constexpr int WS_COUNT = 10;

    namespace output {

        // Per-output state. Standard-layout, so wl_container_of recovers it cleanly from the
        // embedded wl_listeners — keep it that way (no virtuals, no private sections).
        struct Output {
            Server* server = nullptr;
            wlr_output* handle = nullptr;
            wlr_scene_rect* bg = nullptr; // full-output backdrop in the background tree

            // Which workspace is shown here; -1 = none (no workspace assigned to this output).
            // A workspace can *live* on an output (Workspace::output) without being the one
            // shown — that's this field.
            int active_ws = -1;

            // Tiling region left after this output's layer-shell exclusive zones (bars) are
            // subtracted. Layout coordinates, not output-local.
            struct {
                int x, y, width, height;
            } usable_area{};

            bool enabled = true;

            wl_listener frame;
            wl_listener request_state;
            wl_listener destroy;
            timespec last_frame{}; // for frame-rate-independent animation decay
        };

        // Register the backend's new_output listener.
        void register_handlers(Server& server);

        // Output name, e.g. "eDP-1". Empty if the output is gone.
        std::string name_of(const Output* o);

        // True for a laptop's built-in panel by connector name (eDP-*, LVDS-*, DSI-*) — the
        // rule sway/Hyprland use, since wlroots exposes no "is internal" bit. It's a
        // convention, not a guarantee: `lid_controls` lets the config override it.
        bool is_internal(const std::string& name);

        // Whether the lid governs this output: the `lid_output` config if set, else
        // is_internal(). Use this, not is_internal, for policy decisions.
        bool lid_controls(Server& server, const Output* o);

        // Pure workspace-assignment policy, deliberately free of wlroots types so it can be
        // tested without a compositor (see test_output.cpp). Decides which output each of the
        // WS_COUNT workspaces should live on, in place:
        //
        //   home[i]     the output workspace i is configured to prefer ("" = none)
        //   needed[i]   workspace i must be on a screen: it has windows, or it's being shown
        //   live        names of currently enabled outputs, in preference order
        //   current[i]  where workspace i lives now ("" = unassigned); updated in place
        //   origin[i]   the output it was evacuated off ("" = none); updated in place
        //
        // Rules, first match wins:
        //   1. home is live             -> go home (an explicit config always wins)
        //   2. origin is live           -> go back where it was evacuated from; clear origin
        //   3. current is still live    -> stay put; don't churn on unrelated hotplugs
        //   4. needed, some output live -> evacuate to live.front(), remembering origin
        //   5. otherwise                -> "" (unassigned; the tree is kept, nothing renders)
        //
        // `origin` is what makes a lid cycle round-trip with NO config at all: evacuation is
        // something the hardware did TO a workspace, so it records the screen it was taken
        // from and undoes itself when that screen returns. Only rule 4 ever sets it, so a
        // workspace that never lost a screen can't acquire one behind your back.
        //
        // Rule 5 — rather than dumping every idle workspace onto the first output — is what
        // leaves spare workspaces free for a new monitor to claim (see apply_layout). Without
        // it, a second screen has nothing it's allowed to show.
        //
        // This never touches window or tree state — a workspace's BSP tree and its views'
        // workspace index are invariant across every output event. That invariance IS the
        // clamshell guarantee: geometry is recomputed, layout is never rebuilt.
        void assign_workspaces(const std::string home[WS_COUNT],
                               const bool needed[WS_COUNT],
                               const std::vector<std::string>& live,
                               std::string current[WS_COUNT],
                               std::string origin[WS_COUNT]);

        // Enable/disable an output. Disabling evacuates its workspaces, closes its layer
        // surfaces and removes it from the output layout — which destroys its wl_output global,
        // so clients (bars/shells) see the screen genuinely disappear and tear down their
        // per-screen surfaces on their own. Enabling reverses it. No shell reload required.
        void set_enabled(Server& server, Output* o, bool on);

        // THE funnel. Every event that changes the set of outputs — lid toggle, hotplug,
        // output destroy, config reload, the IPC `output` command — calls this and only this.
        // It re-runs the lid policy (which screens should be on), then settles everything:
        // re-home workspaces, reposition outputs, re-arrange, re-clamp the cursor, refocus,
        // publish IPC.
        //
        // It's one call because the cases interact: undocking with the lid shut has to notice
        // the external left AND turn the panel back on. Anything that re-derives only half of
        // that leaves the user staring at a black screen.
        void refresh(Server& server);

        // Settle workspaces/layout/focus for the current set of enabled outputs. Prefer
        // refresh() — this skips the lid policy and is for callers that just changed it.
        void apply_layout(Server& server);

        // Lid policy: with an external screen present, the lid disables the internal panel.
        // fenriz handles only that case; plain suspend is left to logind, whose
        // HandleLidSwitchDocked/HandleLidSwitch defaults already do the right thing (it counts
        // external DRM connectors itself). Does no layout work — refresh() follows it up.
        void apply_lid_policy(Server& server);

        // Apply the config's `output = ...` entries to every live output (mode/position/scale).
        void apply_config(Server& server);

        // Broadcast current head state (mode/scale/position) to wlr-output-management clients
        void publish_heads(Server& server);

        // Find an output by name / by its wlr_output, or null.
        Output* by_name(Server& server, const std::string& name);
        Output* by_handle(Server& server, const wlr_output* handle);

        // This output's effective scale: its `output = ...` entry, else the global `scale`.
        float scale_of(Server& server, const Output* o);

        // The output that should receive new windows / layer surfaces: the focused view's
        // output, else the one under the cursor, else the first. Derived, never stored — one
        // less piece of state to go stale.
        Output* focused(Server& server);

        // Power an output off/on (DPMS). Backs the wlr-output-power-management protocol and
        // the IPC `dpms` command. Null output = all outputs.
        void set_dpms(Server& server, Output* o, bool on);
    } // namespace output

} // namespace fenriz
