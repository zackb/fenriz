#include "config.hpp"

#include <cassert>
#include <cstdio>

using namespace fenriz;

int main() {
    const char* text = "# comment\n"
                       "border_width = 3\n"
                       "rounding = 12  # inline comment\n"
                       "opacity = 0.9\n"
                       "sensitivity = 2.5\n" // out of range -> clamped to 1.0
                       "border_active = 0x33ccffff\n"
                       "exec-once = waybar --config /etc/x\n"
                       "env = QT_QPA_PLATFORMTHEME,qt6ct\n"
                       "env = FOO,a,b,c\n" // value keeps commas; only the first splits
                       "bind = SUPER, Return, exec, foot\n"
                       "bind = SUPER SHIFT, E, exit\n"
                       "bind = SUPER, 2, workspace, 3\n"
                       "bind = SUPER SHIFT, 4, movetoworkspace, 5\n"
                       "garbage line without equals\n";

    Config c = Config::parse(text);

    assert(c.border_width == 3);
    assert(c.rounding == 12);
    assert(c.opacity > 0.89f && c.opacity < 0.91f);
    assert(c.sensitivity == 1.0f); // 2.5 clamped into [-1, 1]
    assert(c.border_active == 0x33ccffffu);

    // exec-once keeps the full command (not comma-split like binds).
    assert(c.exec_once.size() == 1);
    assert(c.exec_once[0] == "waybar --config /etc/x");

    // env splits NAME from VALUE on the first comma only (value may contain commas).
    assert(c.env.size() == 2);
    assert(c.env[0].first == "QT_QPA_PLATFORMTHEME" && c.env[0].second == "qt6ct");
    assert(c.env[1].first == "FOO" && c.env[1].second == "a,b,c");

    assert(c.binds.size() == 4);

    const Bind& b0 = c.binds[0];
    assert(b0.mods == 64u); // SUPER == LOGO
    assert(b0.sym == xkb_keysym_from_name("Return", XKB_KEYSYM_CASE_INSENSITIVE));
    assert(b0.action == Action::Exec);
    assert(b0.arg == "foot");

    const Bind& b1 = c.binds[1];
    assert(b1.mods == (64u | 1u)); // SUPER + SHIFT
    assert(b1.action == Action::Exit);

    // Workspace binds: action + numeric arg preserved for keyboard.cpp to dispatch.
    const Bind& b2 = c.binds[2];
    assert(b2.mods == 64u);
    assert(b2.sym == xkb_keysym_from_name("2", XKB_KEYSYM_CASE_INSENSITIVE));
    assert(b2.action == Action::Workspace);
    assert(b2.arg == "3");

    const Bind& b3 = c.binds[3];
    assert(b3.mods == (64u | 1u));
    assert(b3.action == Action::MoveToWorkspace);
    assert(b3.arg == "5");

    std::printf("config parser: all assertions passed\n");
    return 0;
}
