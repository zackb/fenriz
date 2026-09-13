#pragma once

#include <gtk/gtk.h>

#include <map>
#include <string>
#include <vector>

#include "tray.hpp"

namespace fenriz::bar {

    // Tray icons on each bar. Left click activates (or opens the menu of an item that is only a menu), middle click is
    // the secondary action, right click opens the item's menu, and scrolling goes to the app.
    class TrayUi {
    public:
        explicit TrayUi(Tray& tray);

        TrayUi(const TrayUi&) = delete;
        TrayUi& operator=(const TrayUi&) = delete;

        // A capsule of icons that keeps itself current until it is destroyed with its bar.
        GtkWidget* create();

    private:
        struct Group {
            GtkWidget* box;
            std::map<std::string, GtkWidget*> buttons; // item key -> its icon
        };

        void update();
        void update_group(Group& group);
        GtkWidget* button(const std::string& key);
        void show_menu(GtkWidget* button, const TrayItem& item);
        const TrayItem* item(const std::string& key) const;

        Tray& tray_;
        std::vector<Group*> groups_;
    };

} // namespace fenriz::bar
