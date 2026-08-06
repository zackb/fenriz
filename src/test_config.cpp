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
                       "shadow = off\n"
                       "shadow_color = 0xaabbcc80\n"
                       "shadow_blur = 24\n"
                       "border_gradient = 0xff00aacc\n"
                       "exec-once = waybar --config /etc/x\n"
                       "env = QT_QPA_PLATFORMTHEME,qt6ct\n"
                       "env = FOO,a,b,c\n" // value keeps commas; only the first splits
                       "bind = SUPER, Return, exec, foot\n"
                       "bind = SUPER SHIFT, E, exit\n"
                       "bind = SUPER, 2, workspace, 3\n"
                       "bind = SUPER SHIFT, 4, movetoworkspace, 5\n"
                       "repeat_delay = 300\n"
                       "repeat_rate = 20\n"
                       "zoom_mod = alt\n"
                       "zoom_max = 4.0\n"
                       "binde = , XF86AudioRaiseVolume, exec, wpctl set-volume @X 5%+\n"
                       "garbage line without equals\n";

    Config c = Config::parse(text);

    assert(c.border_width == 3);
    assert(c.rounding == 12);
    assert(c.opacity > 0.89f && c.opacity < 0.91f);
    assert(c.sensitivity == 1.0f); // 2.5 clamped into [-1, 1]
    assert(c.border_active == 0x33ccffffu);
    assert(c.shadow == false);
    assert(c.shadow_color == 0xaabbcc80u);
    assert(c.shadow_blur == 24);
    assert(c.border_gradient == 0xff00aaccu);

    // unset leaves the border flat
    assert(Config::parse("").border_gradient == 0u);

    // exec-once keeps the full command (not comma-split like binds).
    assert(c.exec_once.size() == 1);
    assert(c.exec_once[0] == "waybar --config /etc/x");

    // env splits NAME from VALUE on the first comma only (value may contain commas).
    assert(c.env.size() == 2);
    assert(c.env[0].first == "QT_QPA_PLATFORMTHEME" && c.env[0].second == "qt6ct");
    assert(c.env[1].first == "FOO" && c.env[1].second == "a,b,c");

    assert(c.repeat_delay == 300);
    assert(c.repeat_rate == 20);

    assert(c.zoom_mod == 8u); // "alt" -> WLR_MODIFIER_ALT bit
    assert(c.zoom_max > 3.99f && c.zoom_max < 4.01f);
    assert(Config{}.zoom_mod == 4u); // default is CTRL

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
    assert(junk.scale == 0.0f);

    assert(Config().scale == 0.0f);
    assert(Config::parse("scale = auto\n").scale == 0.0f);
    assert(Config::parse("scale = 1.5\n").scale == 1.5f);

    // Per-output config. Trailing fields are optional and fall back to preferred/auto/global
    // scale, so a bare `output = NAME` is valid.
    Config out = Config::parse("output = eDP-1, preferred, auto, 2.0\n"
                               "output = DP-1, 3840x2160@144, 1920x0, 1.0\n"
                               "output = HDMI-A-1, disable\n"
                               "output = DP-2\n");
    assert(out.outputs.size() == 4);
    assert(out.outputs[0].name == "eDP-1" && out.outputs[0].mode == "preferred");
    assert(out.outputs[0].position == "auto" && out.outputs[0].scale == 2.0f);
    assert(out.outputs[1].mode == "3840x2160@144" && out.outputs[1].position == "1920x0");
    assert(out.outputs[2].mode == "disable");
    // Unspecified fields keep their defaults; scale 0 means "fall back to the global scale".
    assert(out.outputs[3].mode == "preferred" && out.outputs[3].position == "auto" && out.outputs[3].scale == 0);

    Config autos = Config::parse("scale = 2.0\noutput = DP-1, preferred, auto, auto\n");
    assert(autos.scale == 2.0f);
    assert(autos.outputs[0].scale == -1.0f);

    // Workspace homes are 1-indexed in the config, 0-indexed in the array. Out-of-range is
    // ignored rather than writing past the end.
    Config ws = Config::parse("workspace = 1, eDP-1\nworkspace = 10, DP-1\n"
                              "workspace = 0, X\nworkspace = 11, Y\nworkspace = 3\n");
    assert(ws.ws_home[0] == "eDP-1");
    assert(ws.ws_home[9] == "DP-1");
    assert(ws.ws_home[1].empty()); // never set
    assert(ws.ws_home[2].empty()); // `workspace = 3` with no output is ignored

    // A shell command is taken verbatim: it keeps its commas (only the first three
    // separate mods/key/action) and its `#` (which is a hex color or a URL fragment far
    // more often than a comment). Truncating either silently ran the wrong command.
    Config sh = Config::parse("bind = SUPER, Return, exec, sh -c 'a,b'\n"
                              "bind = SUPER, P, exec, grim -o \"$M\" - | satty -f -\n"
                              "exec-once = swaybg -c #1a1a1a\n"
                              "bind = SUPER, C, exec, hyprpicker -f hex # trailing text is part of the command\n");
    assert(sh.binds.size() == 3);
    assert(sh.binds[0].arg == "sh -c 'a,b'");
    assert(sh.binds[1].arg == "grim -o \"$M\" - | satty -f -");
    assert(sh.exec_once.size() == 1 && sh.exec_once[0] == "swaybg -c #1a1a1a");
    assert(sh.binds[2].arg == "hyprpicker -f hex # trailing text is part of the command");

    // ...but a non-command value still honors an inline comment (asserted for `rounding`
    // above) and a whole-line comment is still dropped even though it contains an `=`.
    Config cm = Config::parse("# gaps = 999\n"
                              "  # border_width = 999\n"
                              "gaps = 4 # four\n");
    assert(cm.gaps == 4);
    assert(cm.border_width == Config{}.border_width);

    // Integer settings clamp like the floats do. A negative gap grows tiles past the
    // screen edge; a negative border/rounding/blur reaches the scene rect setters as a
    // negative size. Values are geometry, and geometry has no meaning below zero.
    Config neg = Config::parse("gaps = -5\nborder_width = -3\nrounding = -1\n"
                               "shadow_blur = -10\nanimation = -20\n");
    assert(neg.gaps >= 0);
    assert(neg.border_width >= 0);
    assert(neg.rounding >= 0);
    assert(neg.shadow_blur >= 0);
    assert(neg.animation_ms >= 0);

    // Window rules: name=value fields, any order; `name` is a label and ignored; a rule
    // with neither class nor title is dropped.
    Config wr = Config::parse("windowrule = class=^(org\\.pulseaudio\\.pavucontrol)$, float=true, center=true\n"
                              "windowrule = name=dialogs, title=^$, no_focus=true\n"
                              "windowrule = float=true\n"); // no match field -> dropped
    assert(wr.window_rules.size() == 2);
    assert(wr.window_rules[0].app_id == "^(org\\.pulseaudio\\.pavucontrol)$");
    assert(wr.window_rules[0].floating && wr.window_rules[0].center);
    assert(!wr.window_rules[0].no_focus);
    assert(wr.window_rules[1].title == "^$" && wr.window_rules[1].no_focus);
    assert(!wr.window_rules[1].floating);

    // --- rule matching (what view.cpp does with the parsed rules at map time) ---

    // Every matching rule stacks, and an empty pattern means "any".
    {
        const std::vector<WindowRule> rules = {
            {"^foot$", "", true, false, false},   // class only
            {"", "^scratch$", false, true, false} // title only, matches any class
        };
        const RuleResult foot = match_rules(rules, "foot", "scratch");
        assert(foot.floating && foot.center && !foot.no_focus); // both rules applied
        assert(match_rules(rules, "foot", "vim").floating);
        assert(!match_rules(rules, "foot", "vim").center);
        assert(match_rules(rules, "alacritty", "scratch").center);
        assert(!match_rules(rules, "alacritty", "vim").floating);
    }

    // A null app_id/title reads as "" so `^$` can deliberately target unset identity —
    // XWayland windows routinely map before they set WM_CLASS.
    {
        const std::vector<WindowRule> anon = {{"^$", "", true, false, false}};
        assert(match_rules(anon, nullptr, nullptr).floating);
        assert(match_rules(anon, "", "x").floating);
        assert(!match_rules(anon, "foot", nullptr).floating);

        // An empty ruleset never matches anything, and a rule with two empty patterns
        // matches everything.
        assert(!match_rules({}, "foot", "x").floating);
        const std::vector<WindowRule> all = {{"", "", false, false, true}};
        assert(match_rules(all, nullptr, nullptr).no_focus);
    }

    // An invalid regex matches nothing rather than throwing out of the map handler — the
    // same swallow-don't-crash policy the rest of the parser uses. A typo'd rule must not
    // take the compositor down when a window opens.
    {
        const std::vector<WindowRule> bad = {{"[", "", true, false, false}, {"^ok$", "", false, false, true}};
        const RuleResult r = match_rules(bad, "ok", "");
        assert(!r.floating); // the broken rule matched nothing...
        assert(r.no_focus);  // ...and did not stop the valid one after it
    }

    // Auto-float: a client-placed child (dialog/modal) or a window that declared a fixed
    // size, which a tile would have to violate.
    assert(auto_float(0, 0, 0, 0, true));           // has a parent -> dialog
    assert(auto_float(300, 300, 200, 200, false));  // min == max -> fixed size
    assert(!auto_float(300, 900, 200, 600, false)); // resizable -> tile it
    assert(!auto_float(0, 0, 0, 0, false));         // no hints at all -> tile it
    // A fixed width alone isn't enough; both axes must be pinned.
    assert(!auto_float(300, 300, 200, 600, false));

    std::printf("config parser: all assertions passed\n");
    return 0;
}
