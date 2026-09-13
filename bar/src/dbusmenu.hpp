#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    // One com.canonical.dbusmenu item and what is below it.
    struct MenuNode {
        int id = 0;
        std::string label; // keeps the "_" mnemonic, which GMenu understands the same way
        bool separator = false;
        bool enabled = true;
        bool visible = true;
        enum class Toggle { None, Check, Radio } toggle = Toggle::None;
        bool checked = false;
        std::vector<MenuNode> children;
    };

    // The (ia{sv}av) layout from GetLayout.
    MenuNode parse_layout(GVariant* layout);

    // Builds `root`'s children into `menu`: separators become sections, nodes with children become submenus, and every
    // item gets a "tray.<id>" action in `actions` that calls `clicked(id)`.
    void build_menu(const MenuNode& root, GMenu* menu, GSimpleActionGroup* actions, std::function<void(int)> clicked);

} // namespace fenriz::bar
