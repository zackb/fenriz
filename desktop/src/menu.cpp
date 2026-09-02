#include "menu.hpp"

#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <cstdlib>
#include <map>

#include "spawn.hpp"

namespace fenriz::desktop::menu {

    namespace {

        // Menu order, and the freedesktop main categories each one collects.
        const struct {
            const char* label;
            const char* keys[4];
        } CATEGORIES[] = {
            {"Accessories", {"Utility"}},
            {"Development", {"Development"}},
            {"Education", {"Education"}},
            {"Games", {"Game"}},
            {"Graphics", {"Graphics"}},
            {"Internet", {"Network"}},
            {"Multimedia", {"AudioVideo", "Audio", "Video"}},
            {"Office", {"Office"}},
            {"Science", {"Science"}},
            {"Settings", {"Settings"}},
            {"System", {"System"}},
        };

        constexpr const char* OTHER = "Other";

        void on_exec(GSimpleAction*, GVariant* param, gpointer) {
            const char* command = g_variant_get_string(param, nullptr);
            if (command)
                spawn::command(command);
        }

        void on_launch(GSimpleAction*, GVariant* param, gpointer) {
            const char* id = g_variant_get_string(param, nullptr);
            if (!id)
                return;
            GDesktopAppInfo* info = g_desktop_app_info_new(id);
            if (!info) {
                g_warning("menu: no desktop entry '%s'", id);
                return;
            }
            spawn::app(G_APP_INFO(info));
            g_object_unref(info);
        }

        std::vector<std::string> split_categories(const char* raw) {
            std::vector<std::string> out;
            if (!raw)
                return out;
            char** parts = g_strsplit(raw, ";", -1);
            for (char** p = parts; *p; p++)
                if (**p)
                    out.emplace_back(*p);
            g_strfreev(parts);
            return out;
        }

        void append_exec(GMenu* section, const char* label, const std::string& command) {
            GMenuItem* item = g_menu_item_new(label, nullptr);
            g_menu_item_set_action_and_target_value(item, "app.exec", g_variant_new_string(command.c_str()));
            g_menu_append_item(section, item);
            g_object_unref(item);
        }

    } // namespace

    std::vector<std::string> categories_for(const std::vector<std::string>& categories) {
        std::vector<std::string> out;
        for (const auto& bucket : CATEGORIES)
            for (const char* key : bucket.keys) {
                if (!key)
                    break;
                if (std::find(categories.begin(), categories.end(), key) != categories.end()) {
                    out.emplace_back(bucket.label);
                    break; // one entry per bucket, however many of its keys match
                }
            }
        return out;
    }

    void install_actions(GtkApplication* app) {
        GSimpleAction* exec = g_simple_action_new("exec", G_VARIANT_TYPE_STRING);
        g_signal_connect(exec, "activate", G_CALLBACK(on_exec), nullptr);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(exec));
        g_object_unref(exec);

        GSimpleAction* launch = g_simple_action_new("launch", G_VARIANT_TYPE_STRING);
        g_signal_connect(launch, "activate", G_CALLBACK(on_launch), nullptr);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(launch));
        g_object_unref(launch);
    }

    void show_icons(GtkWidget* widget) {
        for (GtkWidget* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
            if (GTK_IS_IMAGE(child) && gtk_image_get_gicon(GTK_IMAGE(child))) {
                gtk_widget_set_visible(child, TRUE);
                gtk_widget_set_margin_end(child, 8); // the row packs icon and label flush
            }
            show_icons(child);
        }
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

    namespace {

        struct AppEntry {
            std::string id;
            std::string name;
            std::string icon;
        };

        // One submenu per non-empty category, apps sorted by name inside it.
        void append_installed_apps(GMenu* root) {
            std::map<std::string, std::vector<AppEntry>> buckets;

            GList* all = g_app_info_get_all();
            for (GList* l = all; l; l = l->next) {
                auto* info = static_cast<GAppInfo*>(l->data);
                // should_show test NoDisplay, Hidden, and OnlyShowIn/NotShowIn
                if (!g_app_info_should_show(info))
                    continue;
                const char* id = g_app_info_get_id(info);
                const char* name = g_app_info_get_display_name(info);
                if (!id || !name)
                    continue;

                const char* raw =
                    G_IS_DESKTOP_APP_INFO(info) ? g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(info)) : nullptr;
                GIcon* gicon = g_app_info_get_icon(info);
                char* icon = gicon ? g_icon_to_string(gicon) : nullptr;

                std::vector<std::string> labels = categories_for(split_categories(raw));
                if (labels.empty())
                    labels.emplace_back(OTHER);
                for (const std::string& label : labels)
                    buckets[label].push_back({id, name, icon ? icon : ""});
                g_free(icon);
            }
            g_list_free_full(all, g_object_unref);

            if (buckets.empty())
                return;

            GMenu* section = g_menu_new();
            std::vector<std::string> order;
            for (const auto& bucket : CATEGORIES)
                order.emplace_back(bucket.label);
            order.emplace_back(OTHER);

            for (const std::string& label : order) {
                auto it = buckets.find(label);
                if (it == buckets.end())
                    continue;
                std::vector<AppEntry>& apps = it->second;
                std::sort(apps.begin(), apps.end(), [](const AppEntry& a, const AppEntry& b) {
                    const int by_name = g_ascii_strcasecmp(a.name.c_str(), b.name.c_str());
                    return by_name != 0 ? by_name < 0 : a.id < b.id;
                });

                GMenu* submenu = g_menu_new();
                for (const AppEntry& app : apps) {
                    GMenuItem* item = g_menu_item_new(app.name.c_str(), nullptr);
                    if (GIcon* icon = app.icon.empty() ? nullptr : g_icon_new_for_string(app.icon.c_str(), nullptr)) {
                        g_menu_item_set_icon(item, icon);
                        g_object_unref(icon);
                    }
                    g_menu_item_set_action_and_target_value(item, "app.launch", g_variant_new_string(app.id.c_str()));
                    g_menu_append_item(submenu, item);
                    g_object_unref(item);
                }
                g_menu_append_submenu(section, label.c_str(), G_MENU_MODEL(submenu));
                g_object_unref(submenu);
            }

            g_menu_append_section(root, nullptr, G_MENU_MODEL(section));
            g_object_unref(section);
        }

    } // namespace

    GMenuModel* build_model(const Config& cfg) {
        GMenu* root = g_menu_new();

        append_installed_apps(root);

        GMenu* apps = g_menu_new();
        if (cfg.launcher)
            g_menu_append(apps, "Applications", "app.launcher");
        if (!cfg.wallpaper_dir.empty())
            g_menu_append(apps, "Wallpaper", "app.wallpaper");
        if (cfg.notifications && cfg.notify_history > 0)
            g_menu_append(apps, "Notifications", "app.notifications");
        if (std::string terminal = resolve_terminal(cfg); !terminal.empty())
            append_exec(apps, "Terminal", terminal);
        for (const auto& [label, command] : cfg.menu)
            append_exec(apps, label.c_str(), command);
        if (g_menu_model_get_n_items(G_MENU_MODEL(apps)) > 0)
            g_menu_append_section(root, nullptr, G_MENU_MODEL(apps));
        g_object_unref(apps);

        GMenu* power = g_menu_new();
        g_menu_append(power, "Lock", "app.lock");
        g_menu_append(power, "Clean Keyboard", "app.cleaning");
        append_exec(power, "Sleep", "systemctl suspend");
        append_exec(power, "Log Out", logout_command());
        append_exec(power, "Restart", "systemctl reboot");
        append_exec(power, "Shut Down", "systemctl poweroff");
        g_menu_append_submenu(root, "Power", G_MENU_MODEL(power));
        g_object_unref(power);

        return G_MENU_MODEL(root);
    }

} // namespace fenriz::desktop::menu
