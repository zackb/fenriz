#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace fenriz {

    enum class Action {
        None,
        Exec,
        KillActive,
        Exit,
        FocusNext,
        FocusPrev,
        FocusLeft,
        FocusRight,
        FocusUp,
        FocusDown,
        ToggleLayout,
        Fullscreen,
        ToggleFloat,
        Workspace,       // arg = workspace number 1..10
        MoveToWorkspace, // arg = workspace number 1..10
    };

    struct Bind {
        uint32_t mods = 0; // mask of WLR_MODIFIER_* bit values
        xkb_keysym_t sym = XKB_KEY_NoSymbol;
        Action action = Action::None;
        std::string arg;
        bool repeat = false; // re-fire while held (config `binde`), volume/brightness
    };

    // One `output = NAME, mode, position, scale` entry. Unset fields keep their default and
    // fall back to the preferred mode / auto position / the global `scale`.
    struct OutputCfg {
        std::string name;
        std::string mode = "preferred"; // "preferred" | "1920x1080" | "1920x1080@60" | "disable"
        std::string position = "auto";  // "auto" | "1920x0" (layout coords)
        float scale = 0;                // 0 = fall back to Config::scale
    };

    struct Config {
        int border_width = 2;
        uint32_t border_active = 0x33ccffff;   // RGBA
        uint32_t border_inactive = 0x444444ff; // RGBA
        int gaps = 8;
        int rounding = 10;
        int animation_ms = 150; // slide-into-place duration; 0 = instant (no animation)
        float opacity = 1.0f;
        float scale = 1.0f;         // output scale; fractional (e.g. 1.5) supported, 1.0 = off
        bool natural_scroll = true; // libinput scroll direction; false = traditional wheel
        float sensitivity = 0.0f;   // libinput pointer accel speed, -1.0 (slow) .. 1.0 (fast); 0 = default
        bool tap_to_click = true;   // trackpad tap = click (1/2/3 fingers = left/right/middle)
        bool clickfinger = true;    // two-finger press = right-click; false = bottom-right corner
        int repeat_delay = 250;     // ms a `binde` key is held before it starts repeating
        int repeat_rate = 15;       // `binde` fires per second while held
        std::vector<Bind> binds;
        std::vector<std::string> exec_once;                   // commands to run once at startup
        std::vector<std::pair<std::string, std::string>> env; // NAME,VALUE exported before exec-once

        // Per-output settings, by connector name (`output = eDP-1, preferred, auto, 2.0`).
        std::vector<OutputCfg> outputs;
        // Which output the lid controls. Empty = detect by connector name (eDP-/LVDS-/DSI-),
        // which is only a convention: set this when the panel is named something unexpected,
        // or to point the lid at a different screen. Also how clamshell is exercised in a
        // nested session, where outputs are named WL-1.
        std::string lid_output;
        // Each workspace's preferred output name (`workspace = 1, eDP-1`); empty = no
        // preference. A workspace returns home whenever that output is live — this is what
        // makes windows come back to the laptop panel when the lid opens.
        std::string ws_home[10];

        // Parse from a config string. Unknown/malformed lines are ignored.
        static Config parse(const std::string& text);
        // Load from $XDG_CONFIG_HOME/fenriz/fenriz.conf (or ~/.config/...),
        // falling back to built-in defaults if absent.
        static Config load();
        // Full path to the config file (empty if neither XDG_CONFIG_HOME nor HOME is set).
        // Used by load() and by the hot-reload directory watcher.
        static std::string config_path();
    };

    Action action_from_string(const std::string& s);

} // namespace fenriz
