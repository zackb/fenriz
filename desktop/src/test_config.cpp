#include <cassert>
#include <filesystem>

#include "config.hpp"

using fenriz::desktop::Config;

namespace {

    void test_empty() {
        Config c = Config::parse("");
        assert(c.wallpaper.empty());
        assert(c.output_wallpaper.empty());
        // An unconfigured output resolves to "no wallpaper", not to a stray default.
        assert(c.wallpaper_for("eDP-1").empty());
    }

    void test_global_wallpaper() {
        Config c = Config::parse("wallpaper = /pic/a.png\n");
        assert(c.wallpaper == "/pic/a.png");
        assert(c.wallpaper_for("eDP-1") == "/pic/a.png");
        assert(c.wallpaper_for("DP-3") == "/pic/a.png");
    }

    void test_per_output_overrides_global() {
        Config c = Config::parse("wallpaper = /pic/a.png\n"
                                 "output_wallpaper = DP-1, /pic/b.png\n");
        assert(c.wallpaper_for("DP-1") == "/pic/b.png");
        assert(c.wallpaper_for("eDP-1") == "/pic/a.png");
    }

    void test_per_output_without_global() {
        Config c = Config::parse("output_wallpaper = DP-1, /pic/b.png\n");
        assert(c.wallpaper_for("DP-1") == "/pic/b.png");
        assert(c.wallpaper_for("eDP-1").empty());
    }

    void test_wallpaper_dir() {
        setenv("HOME", "/home/nobody", 1);
        Config c = Config::parse("wallpaper_dir = ~/Pictures/walls\n");
        assert(c.wallpaper_dir == "/home/nobody/Pictures/walls");
        // The directory alone is not a wallpaper.
        assert(c.wallpaper_for("eDP-1").empty());
    }

    void test_wallpaper_hook() {
        setenv("HOME", "/home/nobody", 1);
        Config c = Config::parse("wallpaper_hook = ~/bin/wall.sh \"$1\" | tee /tmp/log\n");
        assert(c.wallpaper_hook == "~/bin/wall.sh \"$1\" | tee /tmp/log");
        assert(Config::parse("").wallpaper_hook.empty());
    }

    void test_wallpaper_search() {
        // The picker searches unless it is turned off, and garbage leaves it alone.
        assert(Config::parse("").wallpaper_search);
        assert(!Config::parse("wallpaper_search = off\n").wallpaper_search);
        assert(!Config::parse("wallpaper_search = false\n").wallpaper_search);
        assert(Config::parse("wallpaper_search = on\n").wallpaper_search);
        assert(Config::parse("wallpaper_search = maybe\n").wallpaper_search);
    }

    // A runtime pick is session state and outranks every config key, so that editing
    // the config after picking cannot silently half-apply.
    void test_selected_outranks_config() {
        Config c = Config::parse("wallpaper = /pic/a.png\n"
                                 "output_wallpaper = DP-1, /pic/b.png\n");
        c.selected_wallpaper = "/pic/picked.png";
        assert(c.wallpaper_for("DP-1") == "/pic/picked.png");
        assert(c.wallpaper_for("eDP-1") == "/pic/picked.png");

        c.selected_wallpaper.clear();
        assert(c.wallpaper_for("DP-1") == "/pic/b.png");
        assert(c.wallpaper_for("eDP-1") == "/pic/a.png");
    }

    void test_wallpaper_state_round_trip() {
        setenv("XDG_STATE_HOME", "/tmp/fenriz-desktop-test-state", 1);
        assert(fenriz::desktop::wallpaper_state_path() == "/tmp/fenriz-desktop-test-state/fenriz/wallpaper");

        fenriz::desktop::save_selected_wallpaper("/pic/picked.png");
        assert(fenriz::desktop::load_selected_wallpaper() == "/pic/picked.png");

        // Deleting the state file is the documented way back to the config.
        std::filesystem::remove_all("/tmp/fenriz-desktop-test-state");
        assert(fenriz::desktop::load_selected_wallpaper().empty());
        unsetenv("XDG_STATE_HOME");
    }

    void test_comments_and_blanks() {
        Config c = Config::parse("# a comment\n"
                                 "\n"
                                 "   \n"
                                 "wallpaper = /pic/a.png   # trailing comment\n");
        assert(c.wallpaper == "/pic/a.png");
    }

    void test_garbage_is_ignored() {
        Config c = Config::parse("nonsense\n"
                                 "unknown_key = 3\n"
                                 "output_wallpaper = DP-1\n"     // no path
                                 "output_wallpaper = , /a.png\n" // no output
                                 "wallpaper = /pic/a.png\n");
        assert(c.wallpaper == "/pic/a.png");
        assert(c.output_wallpaper.empty());
    }

    // Only the first comma splits, so a path may contain one.
    void test_comma_in_path() {
        Config c = Config::parse("output_wallpaper = DP-1, /pic/a,b.png\n");
        assert(c.wallpaper_for("DP-1") == "/pic/a,b.png");
    }

    void test_tilde_expansion() {
        setenv("HOME", "/home/tester", 1);
        Config c = Config::parse("wallpaper = ~/pic/a.png\n");
        assert(c.wallpaper == "/home/tester/pic/a.png");
    }

    void test_last_wins() {
        Config c = Config::parse("wallpaper = /pic/a.png\n"
                                 "wallpaper = /pic/b.png\n");
        assert(c.wallpaper == "/pic/b.png");
    }

    void test_terminal() {
        assert(Config::parse("terminal = foot -e tmux\n").terminal == "foot -e tmux");
        assert(Config::parse("").terminal.empty());
    }

    void test_lock_on_suspend() {
        assert(Config::parse("").lock_on_suspend);
        assert(Config::parse("idle_lock = 0\n").lock_on_suspend); // not tied to the idle timers
        assert(!Config::parse("lock_on_suspend = false\n").lock_on_suspend);
        assert(!Config::parse("lock_on_suspend = off\n").lock_on_suspend);
        assert(Config::parse("lock_on_suspend = nonsense\n").lock_on_suspend);
    }

    void test_launcher_toggle() {
        assert(Config::parse("").launcher); // on unless asked otherwise
        assert(!Config::parse("launcher = off\n").launcher);
        assert(!Config::parse("launcher = false\n").launcher);
        assert(!Config::parse("launcher = no\n").launcher);
        assert(!Config::parse("launcher = 0\n").launcher);
        assert(Config::parse("launcher = on\n").launcher);
        assert(Config::parse("launcher = nonsense\n").launcher); // unparseable keeps the default
    }

    void test_menu_entries_keep_order() {
        Config c = Config::parse("menu = Files, nautilus\n"
                                 "menu = Browser, firefox\n");
        assert(c.menu.size() == 2);
        assert(c.menu[0].first == "Files" && c.menu[0].second == "nautilus");
        assert(c.menu[1].first == "Browser" && c.menu[1].second == "firefox");
    }

    // Only the first comma splits, so a shell command may contain commas.
    void test_menu_command_with_comma() {
        Config c = Config::parse("menu = Screenshot, grim -g \"$(slurp)\" - | satty -f -, x\n");
        assert(c.menu.size() == 1);
        assert(c.menu[0].second == "grim -g \"$(slurp)\" - | satty -f -, x");
    }

    void test_menu_garbage_is_ignored() {
        Config c = Config::parse("menu = NoCommand\n"
                                 "menu = , nolabel\n"
                                 "menu = Good, ok\n");
        assert(c.menu.size() == 1);
        assert(c.menu[0].first == "Good");
    }

    // `#` starts a comment, so a command containing one is truncated. Documented, not a bug —
    // this asserts the behaviour so it can't change silently.
    void test_hash_in_command_is_a_comment() {
        Config c = Config::parse("menu = Note, echo hello # trailing\n");
        assert(c.menu.size() == 1);
        assert(c.menu[0].second == "echo hello");
    }

    // No user config falls back to the shipped defaults, so a fresh install still gets a
    // wallpaper and the idle stages rather than a transparent desktop that never sleeps.
    void test_missing_file_falls_back_to_shipped() {
        setenv("XDG_CONFIG_HOME", "/nonexistent-fenriz-desktop-test", 1);
        setenv("XDG_STATE_HOME", "/nonexistent-fenriz-desktop-test", 1);
        Config c = Config::load();
        assert(c.source == Config::default_config_path());
        assert(c.wallpaper.front() == '/');
        assert(c.wallpaper.find('@') == std::string::npos);
        // The three stages must stay in order or the screens go dark while unlocked.
        assert(c.idle_dim > 0 && c.idle_dim < c.idle_lock);
        assert(c.idle_lock > 0 && c.idle_lock < c.idle_dpms);
        unsetenv("XDG_STATE_HOME");
    }

    // Config sits next to fenriz.conf rather than in a directory of its own.
    void test_config_path() {
        setenv("XDG_CONFIG_HOME", "/tmp/config-under-test", 1);
        assert(Config::config_path() == "/tmp/config-under-test/fenriz/fenriz-desktop.conf");

        unsetenv("XDG_CONFIG_HOME");
        setenv("HOME", "/home/nobody", 1);
        assert(Config::config_path() == "/home/nobody/.config/fenriz/fenriz-desktop.conf");
    }

    // shell accent follows the compositor's border colors.
    void test_accents_from_compositor_config() {
        Config cfg;
        cfg.parse_accents("border_width = 2\nborder_active = 0x16b8f3ff\nborder_gradient = 0xff2090ff\n");
        assert(cfg.accent == "#16b8f3");
        assert(cfg.accent_gradient == "#ff2090");

        // Alpha survives, so a translucent border gives a softer ring.
        Config translucent;
        translucent.parse_accents("border_active = 0x16b8f3CC");
        assert(translucent.accent == "rgba(22,184,243,0.800)");

        // A flat border config means a flat accent: both stops the same.
        Config flat;
        flat.parse_accents("border_active = 0xff0000ff\nborder_gradient = 0\n");
        assert(flat.accent == "#ff0000");
        assert(flat.accent_gradient == flat.accent);

        // No border_gradient at all is flat too.
        Config missing;
        missing.parse_accents("border_active = 0xff0000ff\n");
        assert(missing.accent_gradient == "#ff0000");

        // Garbage leaves the brand defaults alone.
        Config garbage;
        garbage.parse_accents("border_active = magenta\n# border_gradient = 0x00ff00ff\n");
        assert(garbage.accent == "#16b8f3");
    }

    void test_shell_opacity() {
        assert(Config::parse("").shell_opacity == 0.80);
        assert(Config::parse("shell_opacity = 0.5\n").shell_opacity == 0.5);
        // 1.0 is the opaque case, and the one that turns the blur request off.
        assert(Config::parse("shell_opacity = 1\n").shell_opacity == 1.0);
        // Out of range clamps rather than producing an invalid CSS alpha.
        assert(Config::parse("shell_opacity = 4\n").shell_opacity == 1.0);
        assert(Config::parse("shell_opacity = -2\n").shell_opacity == 0.0);
        assert(Config::parse("shell_opacity = frosted\n").shell_opacity == 0.80);
    }

} // namespace

int main() {
    test_empty();
    test_global_wallpaper();
    test_per_output_overrides_global();
    test_per_output_without_global();
    test_wallpaper_dir();
    test_wallpaper_hook();
    test_wallpaper_search();
    test_selected_outranks_config();
    test_wallpaper_state_round_trip();
    test_comments_and_blanks();
    test_garbage_is_ignored();
    test_comma_in_path();
    test_tilde_expansion();
    test_last_wins();
    test_terminal();
    test_lock_on_suspend();
    test_launcher_toggle();
    test_menu_entries_keep_order();
    test_menu_command_with_comma();
    test_menu_garbage_is_ignored();
    test_hash_in_command_is_a_comment();
    test_missing_file_falls_back_to_shipped();
    test_config_path();
    test_accents_from_compositor_config();
    test_shell_opacity();
    return 0;
}
