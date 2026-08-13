#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fenriz::desktop {

    // Which screen edges the stack hugs
    struct Anchors {
        bool top = true;
        bool left = false;
        bool right = true;
    };

    // "{top,bottom}-{left,center,right}"; anything else falls back to top-right.
    Anchors notify_anchors(std::string_view position);

    struct Toast {
        guint32 id = 0;
        std::string summary; // plain text
        std::string body;    // valid pango markup
        std::string icon;    // icon theme name or absolute path
        GdkTexture* texture = nullptr;
        // key -> label, "default" excluded
        std::vector<std::pair<std::string, std::string>> actions;
        bool has_default = false;
        bool critical = false;
        int expiry_ms = 0; // 0 never expires
    };

    // The on-screen stack. One layer-shell surface holding a column of toasts.
    class Toasts {
    public:
        using ActionFn = std::function<void(guint32 id, const std::string& key)>;
        using ClosedFn = std::function<void(guint32 id, guint32 reason)>;

        Toasts(std::string position, ActionFn on_action, ClosedFn on_closed);
        ~Toasts();

        Toasts(const Toasts&) = delete;
        Toasts& operator=(const Toasts&) = delete;

        void show(GtkApplication* app, const Toast& toast);
        bool close(guint32 id, guint32 reason);

    private:
        struct Item {
            Toasts* owner = nullptr;
            guint32 id = 0;
            GtkWidget* row = nullptr;
            guint timer = 0;
            bool critical = false;
            bool has_default = false;
        };

        void build(GtkApplication* app);
        GtkWidget* build_row(Item& item, const Toast& toast);
        void arm(Item& item, int expiry_ms);
        void sync_visibility();
        Item* find(guint32 id);

        static gboolean on_expire(gpointer data);
        static gboolean on_faded(gpointer data);
        static void on_click(GtkGestureClick* gesture, int n_press, double x, double y, gpointer data);
        static void on_action_button(GtkButton* button, gpointer data);

        std::string position_;
        ActionFn on_action_;
        ClosedFn on_closed_;
        GtkWindow* window_ = nullptr;
        GtkWidget* column_ = nullptr;
        std::vector<std::unique_ptr<Item>> items_;
    };

} // namespace fenriz::desktop
