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
                       "repeat_delay = 300\n"
                       "repeat_rate = 20\n"
                       "binde = , XF86AudioRaiseVolume, exec, wpctl set-volume @X 5%+\n"
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

    assert(c.repeat_delay == 300);
    assert(c.repeat_rate == 20);

    assert(c.binds.size() == 5);

    const Bind& b0 = c.binds[0];
    assert(b0.mods == 64u); // SUPER == LOGO
    assert(b0.sym == xkb_keysym_from_name("Return", XKB_KEYSYM_CASE_INSENSITIVE));
    assert(b0.action == Action::Exec);
    assert(b0.arg == "foot");
    assert(!b0.repeat); // plain `bind` does not repeat

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

    // `binde` parses like `bind` but flags the bind to repeat while held (no modifier).
    const Bind& b4 = c.binds[4];
    assert(b4.mods == 0u);
    assert(b4.sym == xkb_keysym_from_name("XF86AudioRaiseVolume", XKB_KEYSYM_CASE_INSENSITIVE));
    assert(b4.action == Action::Exec);
    assert(b4.arg == "wpctl set-volume @X 5%+");
    assert(b4.repeat);

    // Out-of-range floats clamp instead of reaching the renderer. opacity feeds
    // wlr_scene_buffer_set_opacity and scale feeds the output/fractional-scale math,
    // where 0, negative, or absurd values misrender rather than fail loudly.
    Config hi = Config::parse("opacity = 5\nscale = 99\n");
    assert(hi.opacity == 1.0f);
    assert(hi.scale == 10.0f);

    Config lo = Config::parse("opacity = -1\nscale = 0\n");
    assert(lo.opacity == 0.0f);
    assert(lo.scale == 0.25f);

    // Garbage leaves the default intact (stof throws, value untouched).
    Config junk = Config::parse("opacity = abc\nscale = \n");
    assert(junk.opacity == 1.0f);
    assert(junk.scale == 1.0f);

    std::printf("config parser: all assertions passed\n");
    return 0;
}
