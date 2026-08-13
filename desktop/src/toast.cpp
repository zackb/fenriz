#include "toast.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>

namespace fenriz::desktop {

    namespace {

        constexpr int WIDTH = 400;
        constexpr int EDGE_MARGIN = 12;
        constexpr int SPACING = 8;
        constexpr int ICON_SIZE = 48;
        constexpr int BODY_LINES = 3;
        // MUST match the .fenriz-toast opacity transition in theme.cpp
        constexpr guint FADE_MS = 200;
        constexpr size_t MAX_VISIBLE = 5;

        constexpr guint32 REASON_EXPIRED = 1;
        constexpr guint32 REASON_DISMISSED = 2;
        constexpr guint32 REASON_UNDEFINED = 4;

        bool looks_like_path(const std::string& icon) {
            return icon.rfind('/', 0) == 0 || icon.rfind("file://", 0) == 0;
        }

    } // namespace

    namespace {

        struct Dismissal {
            Toasts* owner;
            guint32 id;
            guint32 reason;
        };

        gboolean dismiss_idle(gpointer data) {
            std::unique_ptr<Dismissal> dismissal(static_cast<Dismissal*>(data));
            dismissal->owner->close(dismissal->id, dismissal->reason);
            return G_SOURCE_REMOVE;
        }

    } // namespace

    Anchors notify_anchors(std::string_view position) {
        const size_t dash = position.find('-');
        if (dash == std::string_view::npos)
            return {};
        const std::string_view vertical = position.substr(0, dash);
        const std::string_view horizontal = position.substr(dash + 1);
        if ((vertical != "top" && vertical != "bottom") ||
            (horizontal != "left" && horizontal != "right" && horizontal != "center"))
            return {};
        return {vertical == "top", horizontal == "left", horizontal == "right"};
    }

    Toasts::Toasts(std::string position, ActionFn on_action, ClosedFn on_closed)
        : position_(std::move(position)), on_action_(std::move(on_action)), on_closed_(std::move(on_closed)) {}

    Toasts::~Toasts() {
        for (auto& item : items_)
            if (item->timer)
                g_source_remove(item->timer);
        items_.clear();
        if (window_)
            gtk_window_destroy(window_);
    }

    Toasts::Item* Toasts::find(guint32 id) {
        auto it = std::find_if(items_.begin(), items_.end(), [id](const auto& i) { return i->id == id; });
        return it == items_.end() ? nullptr : it->get();
    }

    void Toasts::build(GtkApplication* app) {
        const Anchors anchors = notify_anchors(position_);

        window_ = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, "fenriz-notify");
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        // notification must never take focus away from what you are typing in.
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_window_set_default_size(window_, WIDTH, -1);

        gtk_layer_set_anchor(window_, anchors.top ? GTK_LAYER_SHELL_EDGE_TOP : GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_margin(
            window_, anchors.top ? GTK_LAYER_SHELL_EDGE_TOP : GTK_LAYER_SHELL_EDGE_BOTTOM, EDGE_MARGIN);
        if (anchors.left || anchors.right) {
            const GtkLayerShellEdge edge = anchors.left ? GTK_LAYER_SHELL_EDGE_LEFT : GTK_LAYER_SHELL_EDGE_RIGHT;
            gtk_layer_set_anchor(window_, edge, TRUE);
            gtk_layer_set_margin(window_, edge, EDGE_MARGIN);
        }

        gtk_widget_add_css_class(GTK_WIDGET(window_), "fenriz-notify");

        column_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING);
        gtk_window_set_child(window_, column_);
    }

    void Toasts::on_click(GtkGestureClick*, int, double, double, gpointer data) {
        auto* item = static_cast<Item*>(data);
        Toasts* self = item->owner;
        const guint32 id = item->id;
        if (item->has_default && self->on_action_)
            self->on_action_(id, "default");
        g_idle_add(dismiss_idle, new Dismissal{self, id, REASON_DISMISSED});
    }

    void Toasts::on_action_button(GtkButton* button, gpointer data) {
        auto* item = static_cast<Item*>(data);
        Toasts* self = item->owner;
        const guint32 id = item->id;
        const char* key = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "toast-key"));
        if (key && self->on_action_)
            self->on_action_(id, key);
        g_idle_add(dismiss_idle, new Dismissal{self, id, REASON_DISMISSED});
    }

    GtkWidget* Toasts::build_row(Item& item, const Toast& toast) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(row, "fenriz-toast");
        if (toast.critical)
            gtk_widget_add_css_class(row, "critical");

        GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_box_append(GTK_BOX(row), top);

        if (toast.texture) {
            GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(toast.texture));
            gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
            gtk_widget_set_size_request(picture, ICON_SIZE, ICON_SIZE);
            gtk_widget_set_valign(picture, GTK_ALIGN_START);
            gtk_widget_add_css_class(picture, "toast-icon");
            gtk_box_append(GTK_BOX(top), picture);
        } else if (!toast.icon.empty()) {
            GtkWidget* image = gtk_image_new();
            if (looks_like_path(toast.icon))
                gtk_image_set_from_file(GTK_IMAGE(image), toast.icon.c_str());
            else
                gtk_image_set_from_icon_name(GTK_IMAGE(image), toast.icon.c_str());
            gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_SIZE);
            gtk_widget_set_valign(image, GTK_ALIGN_START);
            gtk_widget_add_css_class(image, "toast-icon");
            gtk_box_append(GTK_BOX(top), image);
        }

        GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_box_append(GTK_BOX(top), text);

        GtkWidget* summary = gtk_label_new(toast.summary.c_str());
        gtk_widget_add_css_class(summary, "toast-summary");
        gtk_label_set_xalign(GTK_LABEL(summary), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(text), summary);

        if (!toast.body.empty()) {
            GtkWidget* body = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(body), toast.body.c_str());
            gtk_widget_add_css_class(body, "toast-body");
            gtk_label_set_xalign(GTK_LABEL(body), 0.0f);
            gtk_label_set_wrap(GTK_LABEL(body), TRUE);
            gtk_label_set_lines(GTK_LABEL(body), BODY_LINES);
            gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(text), body);
        }

        if (!toast.actions.empty()) {
            GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_halign(buttons, GTK_ALIGN_END);
            for (const auto& [key, label] : toast.actions) {
                GtkWidget* button = gtk_button_new_with_label(label.c_str());
                gtk_widget_add_css_class(button, "toast-action");
                g_object_set_data_full(G_OBJECT(button), "toast-key", g_strdup(key.c_str()), g_free);
                g_signal_connect(button, "clicked", G_CALLBACK(on_action_button), &item);
                gtk_box_append(GTK_BOX(buttons), button);
            }
            gtk_box_append(GTK_BOX(row), buttons);
        }

        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
        g_signal_connect(click, "released", G_CALLBACK(on_click), &item);
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

        return row;
    }

    gboolean Toasts::on_expire(gpointer data) {
        auto* item = static_cast<Item*>(data);
        gtk_widget_add_css_class(item->row, "fading");
        item->timer = g_timeout_add(FADE_MS, on_faded, item);
        return G_SOURCE_REMOVE;
    }

    gboolean Toasts::on_faded(gpointer data) {
        auto* item = static_cast<Item*>(data);
        item->timer = 0;
        item->owner->close(item->id, REASON_EXPIRED);
        return G_SOURCE_REMOVE;
    }

    void Toasts::arm(Item& item, int expiry_ms) {
        if (item.timer) {
            g_source_remove(item.timer);
            item.timer = 0;
        }
        gtk_widget_remove_css_class(item.row, "fading");
        if (expiry_ms > 0)
            item.timer = g_timeout_add(static_cast<guint>(expiry_ms), on_expire, &item);
    }

    void Toasts::sync_visibility() {
        if (!window_)
            return;
        if (items_.empty())
            gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
        else
            gtk_window_present(window_);
    }

    void Toasts::show(GtkApplication* app, const Toast& toast) {
        if (!window_)
            build(app);

        Item* item = find(toast.id);
        if (item) {
            gtk_box_remove(GTK_BOX(column_), item->row);
        } else {
            items_.push_back(std::make_unique<Item>());
            item = items_.back().get();
            item->owner = this;
            item->id = toast.id;
        }
        item->critical = toast.critical;
        item->has_default = toast.has_default;
        item->row = build_row(*item, toast);
        gtk_box_append(GTK_BOX(column_), item->row);
        arm(*item, toast.expiry_ms);

        // over the cap, the oldest goes
        while (items_.size() > MAX_VISIBLE) {
            auto victim = std::find_if(items_.begin(), items_.end(), [](const auto& i) { return !i->critical; });
            close((victim == items_.end() ? items_.front() : *victim)->id, REASON_UNDEFINED);
        }

        sync_visibility();
    }

    bool Toasts::close(guint32 id, guint32 reason) {
        auto it = std::find_if(items_.begin(), items_.end(), [id](const auto& i) { return i->id == id; });
        if (it == items_.end())
            return false;

        if ((*it)->timer)
            g_source_remove((*it)->timer);
        gtk_box_remove(GTK_BOX(column_), (*it)->row);
        items_.erase(it);
        sync_visibility();

        if (on_closed_)
            on_closed_(id, reason);
        return true;
    }

} // namespace fenriz::desktop
