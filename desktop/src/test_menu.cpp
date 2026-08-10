#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>

#include "menu.hpp"

using fenriz::desktop::Config;
namespace menu = fenriz::desktop::menu;

namespace {

    struct Item {
        std::string label;
        std::string command; // the "app.exec" target
    };

    std::string attr(GMenuModel* model, int i, const char* name) {
        char* out = nullptr;
        if (!g_menu_model_get_item_attribute(model, i, name, "s", &out))
            return "";
        std::string s = out;
        g_free(out);
        return s;
    }

    std::vector<Item> items_of(GMenuModel* model) {
        std::vector<Item> out;
        for (int i = 0; i < g_menu_model_get_n_items(model); i++)
            out.push_back({attr(model, i, G_MENU_ATTRIBUTE_LABEL), attr(model, i, G_MENU_ATTRIBUTE_TARGET)});
        return out;
    }

    GMenuModel* link_of(GMenuModel* model, int i, const char* link) {
        return g_menu_model_get_item_link(model, i, link);
    }

    // Every item must drive an action the app actually installs; a typo is a dead entry.
    void assert_all_exec(GMenuModel* model) {
        for (int i = 0; i < g_menu_model_get_n_items(model); i++) {
            const std::string action = attr(model, i, G_MENU_ATTRIBUTE_ACTION);
            assert(action == "app.exec" || action == "app.launcher" || action == "app.lock");
        }
    }

    void test_power_submenu_always_present() {
        setenv("PATH", "/nonexistent", 1);
        unsetenv("TERMINAL");
        Config cfg;
        cfg.launcher = false;
        GMenuModel* root = menu::build_model(cfg);

        // No launcher, no terminal, no custom entries: the apps section is dropped entirely.
        assert(g_menu_model_get_n_items(root) == 1);
        assert(attr(root, 0, G_MENU_ATTRIBUTE_LABEL) == "Power");

        GMenuModel* power = link_of(root, 0, G_MENU_LINK_SUBMENU);
        assert(power != nullptr);
        std::vector<Item> p = items_of(power);
        assert(p.size() == 5);
        assert(p[0].label == "Lock");
        assert(attr(power, 0, G_MENU_ATTRIBUTE_ACTION) == "app.lock");
        assert(p[1].label == "Sleep" && p[1].command == "systemctl suspend");
        assert(p[2].label == "Log Out");
        assert(p[3].label == "Restart" && p[3].command == "systemctl reboot");
        assert(p[4].label == "Shut Down" && p[4].command == "systemctl poweroff");
        assert_all_exec(power);

        g_object_unref(power);
        g_object_unref(root);
    }

    void test_terminal_and_custom_entries_in_order() {
        setenv("PATH", "/nonexistent", 1);
        Config cfg;
        cfg.launcher = false;
        cfg.terminal = "kitty";
        cfg.menu = {{"Files", "nautilus"}, {"Shot", "grim -"}};
        GMenuModel* root = menu::build_model(cfg);

        assert(g_menu_model_get_n_items(root) == 2); // apps section, then Power
        GMenuModel* apps = link_of(root, 0, G_MENU_LINK_SECTION);
        assert(apps != nullptr);
        std::vector<Item> a = items_of(apps);
        assert(a.size() == 3);
        assert(a[0].label == "Terminal" && a[0].command == "kitty");
        assert(a[1].label == "Files" && a[1].command == "nautilus");
        assert(a[2].label == "Shot" && a[2].command == "grim -");
        assert_all_exec(apps);

        g_object_unref(apps);
        g_object_unref(root);
    }

    // The launcher entry is on by default and leads the section; `launcher = off` drops it.
    void test_launcher_entry_tracks_config() {
        setenv("PATH", "/nonexistent", 1);
        unsetenv("TERMINAL");

        Config on;
        on.menu = {{"Files", "nautilus"}};
        GMenuModel* root = menu::build_model(on);
        GMenuModel* apps = link_of(root, 0, G_MENU_LINK_SECTION);
        std::vector<Item> a = items_of(apps);
        assert(a.size() == 2);
        assert(a[0].label == "Applications");
        assert(attr(apps, 0, G_MENU_ATTRIBUTE_ACTION) == "app.launcher");
        assert(a[1].label == "Files");
        assert_all_exec(apps);
        g_object_unref(apps);
        g_object_unref(root);

        Config off;
        off.launcher = false;
        off.menu = {{"Files", "nautilus"}};
        GMenuModel* root2 = menu::build_model(off);
        GMenuModel* apps2 = link_of(root2, 0, G_MENU_LINK_SECTION);
        std::vector<Item> b = items_of(apps2);
        assert(b.size() == 1);
        assert(b[0].label == "Files");
        g_object_unref(apps2);
        g_object_unref(root2);
    }

    // Config wins over $TERMINAL, and $TERMINAL is only honoured if it actually exists.
    void test_terminal_resolution_order() {
        setenv("PATH", "/nonexistent", 1);

        Config cfg;
        cfg.terminal = "myterm --flag";
        setenv("TERMINAL", "other", 1);
        assert(menu::resolve_terminal(cfg) == "myterm --flag");

        Config bare;
        setenv("TERMINAL", "definitely-not-installed", 1);
        assert(menu::resolve_terminal(bare).empty()); // not in PATH -> not offered

        unsetenv("TERMINAL");
        assert(menu::resolve_terminal(bare).empty()); // nothing probes successfully either
    }

    // Without a fenriz socket the menu must not offer a fenrizctl that would do nothing.
    void test_logout_falls_back_without_fenriz() {
        unsetenv("FENRIZ_SOCKET");
        assert(menu::logout_command().find("loginctl") != std::string::npos);

        // Socket set but fenrizctl missing from PATH: still must not emit fenrizctl.
        setenv("FENRIZ_SOCKET", "/run/user/1000/fenriz-test.sock", 1);
        setenv("PATH", "/nonexistent", 1);
        assert(menu::logout_command().find("loginctl") != std::string::npos);
        unsetenv("FENRIZ_SOCKET");
    }

} // namespace

int main() {
    test_power_submenu_always_present();
    test_terminal_and_custom_entries_in_order();
    test_launcher_entry_tracks_config();
    test_terminal_resolution_order();
    test_logout_falls_back_without_fenriz();
    return 0;
}
