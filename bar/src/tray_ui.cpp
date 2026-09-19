#include "tray_ui.hpp"

#include <algorithm>

#include "dbusmenu.hpp"
#include "icon.hpp"

namespace fenriz::bar {

    namespace {

        constexpr int ICON_SIZE = 16;

        const char* key_of(GtkWidget* button) {
            return static_cast<const char*>(g_object_get_data(G_OBJECT(button), "tray-key"));
        }

        void set_icon(GtkWidget* image, const TrayItem& item) {
            GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
            if (!item.theme_path.empty()) {
                char** paths = gtk_icon_theme_get_search_path(theme);
                const bool known =
                    paths && g_strv_contains(const_cast<const char* const*>(paths), item.theme_path.c_str());
                g_strfreev(paths);
                if (!known)
                    gtk_icon_theme_add_search_path(theme, item.theme_path.c_str());
            }
            const std::string& name = item.status == "NeedsAttention" && !item.attention_icon_name.empty()
                                          ? item.attention_icon_name
                                          : item.icon_name;
            if (set_image_icon(GTK_IMAGE(image), name))
                return;
            if (item.pixmap)
                gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(item.pixmap));
            else
                gtk_image_set_from_icon_name(GTK_IMAGE(image), "image-missing-symbolic");
        }

    } // namespace

    TrayUi::TrayUi(Tray& tray) : tray_(tray) {
        tray_.subscribe([this] { update(); });
    }

    GtkWidget* TrayUi::create() {
        auto* group = new Group{gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2), {}};
        gtk_widget_add_css_class(group->box, "bar-capsule");
        gtk_widget_add_css_class(group->box, "bar-tray");
        groups_.push_back(group);
        g_object_set_data_full(
            G_OBJECT(group->box), "tray-group", group, [](gpointer data) { delete static_cast<Group*>(data); });
        // the bar owns the box; when it goes, stop updating it
        g_signal_connect(group->box,
                         "destroy",
                         G_CALLBACK(+[](GtkWidget* box, gpointer data) {
                             auto* self = static_cast<TrayUi*>(data);
                             std::erase_if(self->groups_, [box](Group* g) { return g->box == box; });
                         }),
                         this);
        update_group(*group);
        return group->box;
    }

    void TrayUi::update() {
        for (Group* g : groups_)
            update_group(*g);
    }

    const TrayItem* TrayUi::item(const std::string& key) const {
        for (const auto& it : tray_.items())
            if (it->key == key)
                return it.get();
        return nullptr;
    }

    void TrayUi::update_group(Group& group) {
        // passive items are ones the app says need no attention right now
        std::vector<const TrayItem*> shown;
        for (const auto& it : tray_.items())
            if (it->status != "Passive")
                shown.push_back(it.get());

        for (auto it = group.buttons.begin(); it != group.buttons.end();) {
            const bool keep =
                std::any_of(shown.begin(), shown.end(), [&](const TrayItem* t) { return t->key == it->first; });
            if (keep) {
                ++it;
                continue;
            }
            gtk_box_remove(GTK_BOX(group.box), it->second);
            it = group.buttons.erase(it);
        }
        for (const TrayItem* t : shown) {
            GtkWidget*& b = group.buttons[t->key];
            if (!b) {
                b = button(t->key);
                gtk_box_append(GTK_BOX(group.box), b);
            }
            set_icon(gtk_widget_get_first_child(b), *t);
            gtk_widget_set_tooltip_text(b, t->title.empty() ? nullptr : t->title.c_str());
            if (t->status == "NeedsAttention")
                gtk_widget_add_css_class(b, "attention");
            else
                gtk_widget_remove_css_class(b, "attention");
        }
        gtk_widget_set_visible(group.box, !shown.empty());
    }

    GtkWidget* TrayUi::button(const std::string& key) {
        // a plain box, not a GtkButton: a button claims the primary click for itself before this gesture sees it
        GtkWidget* b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(b, "bar-tray-item");
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        GtkWidget* image = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_SIZE);
        gtk_box_append(GTK_BOX(b), image);
        g_object_set_data_full(G_OBJECT(b), "tray-key", g_strdup(key.c_str()), g_free);

        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); // every button
        g_signal_connect(click,
                         "released",
                         G_CALLBACK(+[](GtkGestureClick* g, int, double, double, gpointer data) {
                             auto* self = static_cast<TrayUi*>(data);
                             GtkWidget* b = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
                             const TrayItem* t = self->item(key_of(b));
                             if (!t)
                                 return;
                             switch (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g))) {
                             case GDK_BUTTON_PRIMARY:
                                 if (t->item_is_menu && has_menu(t->menu_path))
                                     self->show_menu(b, *t);
                                 else
                                     self->tray_.activate(*t, 0, 0);
                                 break;
                             case GDK_BUTTON_MIDDLE:
                                 self->tray_.secondary_activate(*t, 0, 0);
                                 break;
                             case GDK_BUTTON_SECONDARY:
                                 if (has_menu(t->menu_path))
                                     self->show_menu(b, *t);
                                 else
                                     self->tray_.context_menu(*t, 0, 0);
                                 break;
                             default:
                                 break;
                             }
                         }),
                         this);
        gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(click));

        GtkEventController* scroll = gtk_event_controller_scroll_new(static_cast<GtkEventControllerScrollFlags>(
            GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
        g_signal_connect(scroll,
                         "scroll",
                         G_CALLBACK(+[](GtkEventControllerScroll* s, double dx, double dy, gpointer data) -> gboolean {
                             auto* self = static_cast<TrayUi*>(data);
                             GtkWidget* b = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(s));
                             if (const TrayItem* t = self->item(key_of(b))) {
                                 const bool horizontal = dx != 0 && dy == 0;
                                 self->tray_.scroll(*t, static_cast<int>(horizontal ? dx : dy), horizontal);
                             }
                             return TRUE;
                         }),
                         this);
        gtk_widget_add_controller(b, scroll);
        return b;
    }

    // The menu is fetched fresh each time it opens, so it is never stale and nothing is kept for items nobody clicks.
    void TrayUi::show_menu(GtkWidget* button, const TrayItem& item) {
        const std::string key = item.key;
        // the button can be rebuilt away while the reply is on its way
        GWeakRef* weak = g_new0(GWeakRef, 1);
        g_weak_ref_init(weak, button);
        tray_.fetch_menu(item, [this, key, weak](GVariant* layout) {
            GtkWidget* b = GTK_WIDGET(g_weak_ref_get(weak));
            g_weak_ref_clear(weak);
            g_free(weak);
            if (!b)
                return;
            GMenu* menu = g_menu_new();
            GSimpleActionGroup* actions = g_simple_action_group_new();
            build_menu(parse_layout(layout), menu, actions, [this, key](int id) {
                if (const TrayItem* t = this->item(key))
                    tray_.menu_clicked(*t, id);
            });
            gtk_widget_insert_action_group(b, "tray", G_ACTION_GROUP(actions));

            GtkWidget* popover = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(b), "tray-popover"));
            if (!popover) {
                popover = gtk_popover_menu_new_from_model(nullptr);
                gtk_widget_set_parent(popover, b);
                gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
                g_object_set_data_full(
                    G_OBJECT(b), "tray-popover", popover, [](gpointer p) { gtk_widget_unparent(GTK_WIDGET(p)); });
            }
            gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(popover), G_MENU_MODEL(menu));
            gtk_popover_popup(GTK_POPOVER(popover));
            g_object_unref(menu);
            g_object_unref(actions);
            g_object_unref(b);
        });
    }

} // namespace fenriz::bar
