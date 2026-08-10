#include "menu.hpp"

#include <cstdlib>

#include "spawn.hpp"

namespace fenriz::desktop::menu {

    namespace {

        void on_exec(GSimpleAction*, GVariant* param, gpointer) {
            const char* command = g_variant_get_string(param, nullptr);
            if (command)
                spawn::command(command);
        }

        void append_exec(GMenu* section, const char* label, const std::string& command) {
            GMenuItem* item = g_menu_item_new(label, nullptr);
            g_menu_item_set_action_and_target_value(item, "app.exec", g_variant_new_string(command.c_str()));
            g_menu_append_item(section, item);
            g_object_unref(item);
        }

    } // namespace

    void install_actions(GtkApplication* app) {
        GSimpleAction* exec = g_simple_action_new("exec", G_VARIANT_TYPE_STRING);
        g_signal_connect(exec, "activate", G_CALLBACK(on_exec), nullptr);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(exec));
        g_object_unref(exec);
    }

    std::string resolve_terminal(const Config& cfg) {
        auto in_path = [](const char* name) {
            char* found = g_find_program_in_path(name);
            bool ok = found != nullptr;
            g_free(found);
            return ok;
        };

        if (!cfg.terminal.empty())
            return cfg.terminal; // trusted verbatim: it may be a full command line
        if (const char* env = std::getenv("TERMINAL"); env && *env && in_path(env))
            return env;
        for (const char* candidate : {"foot", "ghostty", "kitty", "alacritty", "wezterm", "xterm"})
            if (in_path(candidate))
                return candidate;
        return "";
    }

    std::string logout_command() {
        // fenrizctl only means anything when a fenriz is actually listening.
        if (const char* sock = std::getenv("FENRIZ_SOCKET"); sock && *sock) {
            char* found = g_find_program_in_path("fenrizctl");
            bool ok = found != nullptr;
            g_free(found);
            if (ok)
                return "fenrizctl exit";
        }
        return "loginctl terminate-session \"$XDG_SESSION_ID\"";
    }

    GMenuModel* build_model(const Config& cfg) {
        GMenu* root = g_menu_new();

        GMenu* apps = g_menu_new();
        if (cfg.launcher)
            g_menu_append(apps, "Applications", "app.launcher");
        if (std::string terminal = resolve_terminal(cfg); !terminal.empty())
            append_exec(apps, "Terminal", terminal);
        for (const auto& [label, command] : cfg.menu)
            append_exec(apps, label.c_str(), command);
        if (g_menu_model_get_n_items(G_MENU_MODEL(apps)) > 0)
            g_menu_append_section(root, nullptr, G_MENU_MODEL(apps));
        g_object_unref(apps);

        GMenu* power = g_menu_new();
        g_menu_append(power, "Lock", "app.lock");
        append_exec(power, "Sleep", "systemctl suspend");
        append_exec(power, "Log Out", logout_command());
        append_exec(power, "Restart", "systemctl reboot");
        append_exec(power, "Shut Down", "systemctl poweroff");
        g_menu_append_submenu(root, "Power", G_MENU_MODEL(power));
        g_object_unref(power);

        return G_MENU_MODEL(root);
    }

} // namespace fenriz::desktop::menu
