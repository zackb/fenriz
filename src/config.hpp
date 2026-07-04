#pragma once

#include <cstdint>
#include <string>
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
        Workspace,       // arg = workspace number 1..10
        MoveToWorkspace, // arg = workspace number 1..10
    };

    struct Bind {
        uint32_t mods = 0; // mask of WLR_MODIFIER_* bit values
        xkb_keysym_t sym = XKB_KEY_NoSymbol;
        Action action = Action::None;
        std::string arg;
    };

    struct Config {
        int border_width = 2;
        uint32_t border_active = 0x33ccffff;   // RGBA
        uint32_t border_inactive = 0x444444ff; // RGBA
        int gaps = 8;
        int rounding = 10;
        float opacity = 1.0f;
        float scale = 1.0f; // output scale; fractional (e.g. 1.5) supported, 1.0 = off
        std::vector<Bind> binds;
        std::vector<std::string> exec_once; // commands to run once at startup

        // Parse from a config string. Unknown/malformed lines are ignored.
        static Config parse(const std::string& text);
        // Load from $XDG_CONFIG_HOME/fenriz/fenriz.conf (or ~/.config/...),
        // falling back to built-in defaults if absent.
        static Config load();
    };

    Action action_from_string(const std::string& s);

} // namespace fenriz
