#include "history.hpp"

#include <gtk4-layer-shell.h>

#include <memory>

#include "blur.hpp"
#include "toast.hpp"

namespace fenriz::desktop {

    namespace {

        constexpr int WIDTH = 400;
        constexpr int EDGE_MARGIN = 12;
        constexpr int ICON_SIZE = 32;
        constexpr int BODY_LINES = 2;
        // Fraction of the screen the list may fill before it scrolls.
        constexpr double MAX_LIST_FRACTION = 0.6;
        constexpr int FALLBACK_LIST_HEIGHT = 420;

        bool looks_like_path(const std::string& icon) {
            return icon.rfind('/', 0) == 0 || icon.rfind("file://", 0) == 0;
        }

        int max_list_height() {
            GdkDisplay* display = gdk_display_get_default();
            GListModel* monitors = display ? gdk_display_get_monitors(display) : nullptr;
            auto* monitor = static_cast<GdkMonitor*>(monitors ? g_list_model_get_item(monitors, 0) : nullptr);
            if (!monitor)
                return FALLBACK_LIST_HEIGHT;
            GdkRectangle geometry;
            gdk_monitor_get_geometry(monitor, &geometry);
            g_object_unref(monitor);
            return static_cast<int>(geometry.height * MAX_LIST_FRACTION);
        }

        // "Slack · 3:04 PM"
        std::string footer_text(const HistoryItem& item) {
            GDateTime* when = g_date_time_new_from_unix_local(item.time / 1000);
            char* clock = when ? g_date_time_format(when, "%l:%M %p") : nullptr;
            std::string out = item.app_name;
            if (clock) {
                if (!out.empty())
                    out += " · ";
                out += g_strstrip(clock);
            }
            g_free(clock);
            if (when)
                g_date_time_unref(when);
            return out;
        }

        // Dismissing rebuilds the list, so the button cannot be destroyed inside its own "clicked" handler.
        struct Dismissal {
            History* owner;
            guint32 id;
        };

    } // namespace

    History::History(const Config& cfg, Notifications& notifications) : cfg_(cfg), notifications_(notifications) {
        notifications_.set_history_changed([this] {
            if (window_ && gtk_widget_get_visible(GTK_WIDGET(window_)))
                refill();
        });
    }

    History::~History() {
        notifications_.set_history_changed(nullptr);
        if (window_)
            gtk_window_destroy(window_);
    }

    gboolean History::dismiss_idle(gpointer data) {
        std::unique_ptr<Dismissal> dismissal(static_cast<Dismissal*>(data));
        dismissal->owner->notifications_.dismiss_history(dismissal->id); // refills via the callback
        return G_SOURCE_REMOVE;
    }

    void History::on_dismiss(GtkButton* button, gpointer data) {
        auto* self = static_cast<History*>(data);
        const auto id = static_cast<guint32>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "history-id")));
        g_idle_add(dismiss_idle, new Dismissal{self, id});
    }

    void History::on_clear(GtkButton*, gpointer data) {
        auto* self = static_cast<History*>(data);
        self->notifications_.clear_history();
        self->close();
    }

    gboolean History::on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
        if (keyval != GDK_KEY_Escape)
            return FALSE;
        static_cast<History*>(data)->close();
        return TRUE;
    }

    void History::build(GtkApplication* app) {
        const Anchors anchors = notify_anchors(cfg_.notify_position);

        window_ = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, "fenriz-notify-history");
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_window_set_default_size(window_, WIDTH, -1);

        // Same corner the toasts stack in, so it opens where you were already looking.
        gtk_layer_set_anchor(window_, anchors.top ? GTK_LAYER_SHELL_EDGE_TOP : GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_margin(
            window_, anchors.top ? GTK_LAYER_SHELL_EDGE_TOP : GTK_LAYER_SHELL_EDGE_BOTTOM, EDGE_MARGIN);
        if (anchors.left || anchors.right) {
            const GtkLayerShellEdge edge = anchors.left ? GTK_LAYER_SHELL_EDGE_LEFT : GTK_LAYER_SHELL_EDGE_RIGHT;
            gtk_layer_set_anchor(window_, edge, TRUE);
            gtk_layer_set_margin(window_, edge, EDGE_MARGIN);
        }
        gtk_widget_add_css_class(GTK_WIDGET(window_), "fenriz-shell");

        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(root, "fenriz-notify-history");

        GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(header, "history-header");

        GtkWidget* title = gtk_label_new("Notifications");
        gtk_widget_add_css_class(title, "history-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_set_hexpand(title, TRUE);
        gtk_box_append(GTK_BOX(header), title);

        GtkWidget* clear = gtk_button_new_with_label("Clear all");
        gtk_widget_add_css_class(clear, "history-clear");
        g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), this);
        gtk_box_append(GTK_BOX(header), clear);
        gtk_box_append(GTK_BOX(root), header);

        empty_ = gtk_label_new("No notifications");
        gtk_widget_add_css_class(empty_, "history-empty");
        gtk_box_append(GTK_BOX(root), empty_);

        list_ = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(list_, "history-list");

        scroll_ = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), list_);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll_), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll_), max_list_height());
        gtk_box_append(GTK_BOX(root), scroll_);

        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), this);
        gtk_widget_add_controller(GTK_WIDGET(window_), keys);

        gtk_window_set_child(window_, root);
        blur::attach(GTK_NATIVE(window_));
    }

    void History::refill() {
        const std::deque<HistoryItem>& items = notifications_.history();

        gtk_widget_set_visible(empty_, items.empty());
        gtk_widget_set_visible(scroll_, !items.empty());

        gtk_list_box_remove_all(GTK_LIST_BOX(list_));
        for (const HistoryItem& item : items) {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_widget_add_css_class(box, "history-item");
            if (item.critical)
                gtk_widget_add_css_class(box, "critical");

            if (item.texture) {
                GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(item.texture));
                gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
                gtk_widget_set_size_request(picture, ICON_SIZE, ICON_SIZE);
                gtk_widget_set_valign(picture, GTK_ALIGN_START);
                gtk_widget_add_css_class(picture, "history-icon");
                gtk_box_append(GTK_BOX(box), picture);
            } else if (!item.icon.empty()) {
                GtkWidget* image = gtk_image_new();
                if (looks_like_path(item.icon))
                    gtk_image_set_from_file(GTK_IMAGE(image), item.icon.c_str());
                else
                    gtk_image_set_from_icon_name(GTK_IMAGE(image), item.icon.c_str());
                gtk_image_set_pixel_size(GTK_IMAGE(image), ICON_SIZE);
                gtk_widget_set_valign(image, GTK_ALIGN_START);
                gtk_widget_add_css_class(image, "history-icon");
                gtk_box_append(GTK_BOX(box), image);
            }

            GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_hexpand(text, TRUE);
            gtk_box_append(GTK_BOX(box), text);

            GtkWidget* summary = gtk_label_new(item.summary.c_str());
            gtk_widget_add_css_class(summary, "history-summary");
            gtk_label_set_xalign(GTK_LABEL(summary), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(text), summary);

            if (!item.body.empty()) {
                GtkWidget* body = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(body), item.body.c_str());
                gtk_widget_add_css_class(body, "history-body");
                gtk_label_set_xalign(GTK_LABEL(body), 0.0f);
                gtk_label_set_wrap(GTK_LABEL(body), TRUE);
                gtk_label_set_lines(GTK_LABEL(body), BODY_LINES);
                gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
                gtk_box_append(GTK_BOX(text), body);
            }

            const std::string footer = footer_text(item);
            if (!footer.empty()) {
                GtkWidget* label = gtk_label_new(footer.c_str());
                gtk_widget_add_css_class(label, "history-footer");
                gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
                gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
                gtk_box_append(GTK_BOX(text), label);
            }

            GtkWidget* dismiss = gtk_button_new_from_icon_name("window-close-symbolic");
            gtk_widget_add_css_class(dismiss, "history-dismiss");
            gtk_widget_add_css_class(dismiss, "flat");
            gtk_widget_set_valign(dismiss, GTK_ALIGN_CENTER);
            g_object_set_data(G_OBJECT(dismiss), "history-id", GUINT_TO_POINTER(item.id));
            g_signal_connect(dismiss, "clicked", G_CALLBACK(on_dismiss), this);
            gtk_box_append(GTK_BOX(box), dismiss);

            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
            gtk_list_box_append(GTK_LIST_BOX(list_), row);
        }
    }

    void History::toggle(GtkApplication* app) {
        if (window_ && gtk_widget_get_visible(GTK_WIDGET(window_))) {
            close();
            return;
        }
        if (!window_)
            build(app);
        refill();
        gtk_window_present(window_);
    }

    void History::close() {
        if (window_)
            gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }

} // namespace fenriz::desktop
