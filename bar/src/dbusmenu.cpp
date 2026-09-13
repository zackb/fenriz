#include "dbusmenu.hpp"

#include <memory>

namespace fenriz::bar {

    MenuNode parse_layout(GVariant* layout) {
        MenuNode node;
        if (!layout || !g_variant_is_of_type(layout, G_VARIANT_TYPE("(ia{sv}av)")))
            return node;

        GVariant* props = nullptr;
        GVariant* children = nullptr;
        g_variant_get(layout, "(i@a{sv}@av)", &node.id, &props, &children);

        const char* s = nullptr;
        gboolean b = FALSE;
        gint32 i = 0;
        if (g_variant_lookup(props, "label", "&s", &s))
            node.label = s;
        if (g_variant_lookup(props, "type", "&s", &s))
            node.separator = g_strcmp0(s, "separator") == 0;
        if (g_variant_lookup(props, "enabled", "b", &b))
            node.enabled = b;
        if (g_variant_lookup(props, "visible", "b", &b))
            node.visible = b;
        if (g_variant_lookup(props, "toggle-type", "&s", &s)) {
            if (g_strcmp0(s, "checkmark") == 0)
                node.toggle = MenuNode::Toggle::Check;
            else if (g_strcmp0(s, "radio") == 0)
                node.toggle = MenuNode::Toggle::Radio;
        }
        if (g_variant_lookup(props, "toggle-state", "i", &i))
            node.checked = i == 1;

        GVariantIter it;
        g_variant_iter_init(&it, children);
        while (GVariant* child = g_variant_iter_next_value(&it)) {
            GVariant* inner = g_variant_get_variant(child);
            node.children.push_back(parse_layout(inner));
            g_variant_unref(inner);
            g_variant_unref(child);
        }
        g_variant_unref(props);
        g_variant_unref(children);
        return node;
    }

    void build_menu(const MenuNode& root, GMenu* menu, GSimpleActionGroup* actions, std::function<void(int)> clicked) {
        auto shared = std::make_shared<std::function<void(int)>>(std::move(clicked));
        GMenu* section = g_menu_new();
        auto close_section = [&] {
            if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
                g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
        };

        for (const MenuNode& n : root.children) {
            if (!n.visible)
                continue;
            if (n.separator) {
                close_section();
                continue;
            }
            const std::string name = std::to_string(n.id);
            const std::string detailed = "tray." + name;
            if (!n.children.empty()) {
                GMenu* sub = g_menu_new();
                build_menu(n, sub, actions, *shared);
                g_menu_append_submenu(section, n.label.c_str(), G_MENU_MODEL(sub));
                g_object_unref(sub);
                continue;
            }

            GSimpleAction* action =
                n.toggle == MenuNode::Toggle::None
                    ? g_simple_action_new(name.c_str(), nullptr)
                    : g_simple_action_new_stateful(name.c_str(), nullptr, g_variant_new_boolean(n.checked));
            g_simple_action_set_enabled(action, n.enabled);
            // The app owns the toggle state and reports it back with the next layout; nothing to flip here.
            g_signal_connect_data(
                action,
                "activate",
                G_CALLBACK(+[](GSimpleAction* a, GVariant*, gpointer data) {
                    const auto* fn = static_cast<std::shared_ptr<std::function<void(int)>>*>(data);
                    (**fn)(std::stoi(g_action_get_name(G_ACTION(a))));
                }),
                new std::shared_ptr<std::function<void(int)>>(shared),
                +[](gpointer data, GClosure*) { delete static_cast<std::shared_ptr<std::function<void(int)>>*>(data); },
                GConnectFlags(0));
            g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
            g_object_unref(action);
            g_menu_append(section, n.label.c_str(), detailed.c_str());
        }
        close_section();
        g_object_unref(section);
    }

} // namespace fenriz::bar
