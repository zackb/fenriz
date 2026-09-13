#include <cassert>

#include "dbusmenu.hpp"
#include "tray.hpp"

using namespace fenriz::bar;

namespace {

    GVariant* parse(const char* text) {
        GError* err = nullptr;
        GVariant* v = g_variant_parse(nullptr, text, nullptr, nullptr, &err);
        assert(v && !err);
        return g_variant_ref_sink(v);
    }

    constexpr const char* LAYOUT =
        "(0, {'children-display': <'submenu'>}, ["
        "  <(1, {'label': <'_Open'>}, @av [])>,"
        "  <(2, {'type': <'separator'>}, @av [])>,"
        "  <(3, {'label': <'Mute'>, 'toggle-type': <'checkmark'>, 'toggle-state': <1>}, @av [])>,"
        "  <(4, {'label': <'Hidden'>, 'visible': <false>}, @av [])>,"
        "  <(5, {'label': <'More'>, 'children-display': <'submenu'>}, ["
        "      <(6, {'label': <'Deep'>, 'enabled': <false>}, @av [])>])>"
        "])";

    void test_parse_layout() {
        GVariant* v = parse(LAYOUT);
        const MenuNode root = parse_layout(v);
        g_variant_unref(v);
        assert(root.id == 0);
        assert(root.children.size() == 5);
        assert(root.children[0].label == "_Open");
        assert(root.children[1].separator);
        assert(root.children[2].toggle == MenuNode::Toggle::Check && root.children[2].checked);
        assert(!root.children[3].visible);
        assert(root.children[4].children.size() == 1);
        assert(!root.children[4].children[0].enabled);
        assert(parse_layout(nullptr).children.empty());
    }

    void test_build_menu() {
        GVariant* v = parse(LAYOUT);
        const MenuNode root = parse_layout(v);
        g_variant_unref(v);

        GMenu* menu = g_menu_new();
        GSimpleActionGroup* actions = g_simple_action_group_new();
        int clicked = -1;
        build_menu(root, menu, actions, [&](int id) { clicked = id; });

        // two sections around the separator; the hidden item is left out
        assert(g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 2);
        GMenuModel* second = g_menu_model_get_item_link(G_MENU_MODEL(menu), 1, G_MENU_LINK_SECTION);
        assert(g_menu_model_get_n_items(second) == 2); // Mute, More
        g_object_unref(second);

        assert(g_action_group_has_action(G_ACTION_GROUP(actions), "1"));
        assert(!g_action_group_has_action(G_ACTION_GROUP(actions), "4"));
        assert(!g_action_group_get_action_enabled(G_ACTION_GROUP(actions), "6"));
        GVariant* state = g_action_group_get_action_state(G_ACTION_GROUP(actions), "3");
        assert(state && g_variant_get_boolean(state));
        g_variant_unref(state);

        g_action_group_activate_action(G_ACTION_GROUP(actions), "1", nullptr);
        assert(clicked == 1);

        g_object_unref(actions); // frees the click closures with it
        g_object_unref(menu);
    }

    void test_item_address() {
        auto a = parse_item_address("org.kde.StatusNotifierItem-111648-1/StatusNotifierItem", ":1.9");
        assert(a.bus == "org.kde.StatusNotifierItem-111648-1" && a.path == "/StatusNotifierItem");
        a = parse_item_address(":1.42", ":1.42");
        assert(a.bus == ":1.42" && a.path == "/StatusNotifierItem");
        // Ayatana-style: only a path, the sender is the bus name
        a = parse_item_address("/org/ayatana/NotificationItem/nm_applet", ":1.77");
        assert(a.bus == ":1.77" && a.path == "/org/ayatana/NotificationItem/nm_applet");
    }

    void test_pick_pixmap() {
        assert(pick_pixmap({}, 32) == -1);
        assert(pick_pixmap({16, 22, 32, 64}, 32) == 2);
        assert(pick_pixmap({16, 64}, 32) == 1); // the smallest one that is big enough
        assert(pick_pixmap({16, 22}, 32) == 1); // else the biggest
        assert(pick_pixmap({48, 24, 96}, 32) == 0);
    }

    void test_menu_path() {
        assert(has_menu("/MenuBar"));
        assert(!has_menu("/NO_DBUSMENU")); // what libappindicator-free GTK apps report
        assert(!has_menu(""));
        assert(!has_menu("/"));
    }

} // namespace

int main() {
    test_parse_layout();
    test_build_menu();
    test_item_address();
    test_pick_pixmap();
    test_menu_path();
    return 0;
}
