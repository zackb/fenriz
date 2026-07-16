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
        int repeat_delay = 250;     // ms a `binde` key is held before it starts repeating
        int repeat_rate = 15;       // `binde` fires per second while held
        std::vector<Bind> binds;
        std::vector<std::string> exec_once;                   // commands to run once at startup
        std::vector<std::pair<std::string, std::string>> env; // NAME,VALUE exported before exec-once

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
