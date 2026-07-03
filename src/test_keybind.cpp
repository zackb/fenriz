#include "config.hpp"

#include <cassert>
#include <cstdio>
#include <xkbcommon/xkbcommon.h>

using namespace fenriz;

// Base-level (unshifted) keysym for an xkb keycode, mirroring how keyboard.cpp matches
// binds (xkb_keymap_key_get_syms_by_level at level 0) so SHIFT-modified binds still
// resolve to the base symbol.
static xkb_keysym_t base_sym(xkb_keymap* km, xkb_keycode_t kc) {
    const xkb_keysym_t* syms;
    int n = xkb_keymap_key_get_syms_by_level(km, kc, 0, 0, &syms);
    return n > 0 ? syms[0] : XKB_KEY_NoSymbol;
}

int main() {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names names = {};
    names.layout = "us"; // pin the layout so the test is env-independent
    xkb_keymap* km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(km);

    // The bind matcher compares the config-parsed sym against the base-level sym of the
    // pressed key, plus the modifier mask. evdev KEY_T=20 -> xkb 28, KEY_E=18 -> xkb 26.
    Config c = Config::parse("bind = SUPER, T, exec, foo\n"
                             "bind = SUPER SHIFT, E, exit\n");
    assert(c.binds.size() == 2);

    // Plain letter: SUPER+t resolves to the same sym the config stored.
    assert(c.binds[0].sym == base_sym(km, 28)); // 't'
    assert(c.binds[0].mods == 64u);             // LOGO

    // Shifted bind: config lowercases via CASE_INSENSITIVE, and we match the base-level
    // (unshifted) sym, so SUPER+SHIFT+E matches XKB_KEY_e (not XKB_KEY_E).
    assert(c.binds[1].sym == base_sym(km, 26)); // 'e'
    assert(c.binds[1].sym == XKB_KEY_e);
    assert(c.binds[1].mods == (64u | 1u)); // LOGO|SHIFT

    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    std::printf("keybind matching: all assertions passed\n");
    return 0;
}
