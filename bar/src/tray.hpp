#pragma once

#include <gdk/gdk.h>
#include <gio/gio.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fenriz::bar {

    struct ItemAddress {
        std::string bus;
        std::string path;
    };

    // A registered item as a watcher lists it: "bus/path", a bare bus name, or (Ayatana) a bare path owned by `sender`.
    ItemAddress parse_item_address(const std::string& service, const std::string& sender);

    // Index of the icon to draw at `wanted` px from the pixmaps' sizes: the smallest at least that big, else the
    // biggest.
    int pick_pixmap(const std::vector<int>& sizes, int wanted);

    // False for the placeholder paths apps use to say they have no menu.
    bool has_menu(const std::string& path);

    struct TrayItem {
        std::string key; // bus + path
        std::string bus;
        std::string path;
        std::string title;
        std::string status = "Active"; // Passive, Active, NeedsAttention
        std::string icon_name;
        std::string theme_path; // IconThemePath, an extra directory to find icon_name in
        std::string attention_icon_name;
        GdkTexture* pixmap = nullptr; // IconPixmap, when there is no themed icon
        std::string menu_path;
        bool item_is_menu = false;
        guint subscription = 0; // its New* signals

        ~TrayItem() {
            if (pixmap)
                g_object_unref(pixmap);
        }
    };

    // The system tray: a StatusNotifierWatcher of our own when the name is free, or a host of whoever holds it
    // (quickshell, waybar, KDE). Either way it follows every registered StatusNotifierItem.
    class Tray {
    public:
        Tray() = default;
        ~Tray();

        Tray(const Tray&) = delete;
        Tray& operator=(const Tray&) = delete;

        void start(GDBusConnection* bus);
        void subscribe(std::function<void()> listener);

        const std::vector<std::unique_ptr<TrayItem>>& items() const { return items_; }

        void activate(const TrayItem& item, int x, int y);
        void secondary_activate(const TrayItem& item, int x, int y);
        void scroll(const TrayItem& item, int delta, bool horizontal);
        // The app's own menu, for items without a DBusMenu.
        void context_menu(const TrayItem& item, int x, int y);

        // Asks for the item's menu layout and hands it over; nothing is called when it has none or the call fails.
        void fetch_menu(const TrayItem& item, std::function<void(GVariant* layout)> done);
        void menu_clicked(const TrayItem& item, int id);

    private:
        void become_host();
        void add(const std::string& service, const std::string& sender);
        void remove(const std::string& bus);
        void refresh(const std::string& key);
        TrayItem* find(const std::string& key);
        void notify();
        void emit(const char* signal, const std::string& service);

        static void on_watcher_method(GDBusConnection* bus,
                                      const char* sender,
                                      const char* path,
                                      const char* iface,
                                      const char* method,
                                      GVariant* params,
                                      GDBusMethodInvocation* invocation,
                                      gpointer data);
        static GVariant* on_watcher_property(GDBusConnection* bus,
                                             const char* sender,
                                             const char* path,
                                             const char* iface,
                                             const char* property,
                                             GError** error,
                                             gpointer data);

        GDBusConnection* bus_ = nullptr;
        GCancellable* cancel_ = nullptr;
        guint watch_id_ = 0; // who holds org.kde.StatusNotifierWatcher
        guint watcher_name_ = 0;
        guint host_name_ = 0;
        bool is_watcher_ = false;
        std::string owner_; // unique name of the watcher we last saw
        guint watcher_object_ = 0;
        guint owner_changed_ = 0;
        std::vector<guint> subscriptions_;
        std::vector<std::unique_ptr<TrayItem>> items_;
        std::vector<std::function<void()>> listeners_;
    };

} // namespace fenriz::bar
