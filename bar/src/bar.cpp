#include "bar.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>

#include "blur.hpp"
#include "share.hpp"
#include "volume.hpp"

namespace fenriz::bar {

    namespace {

        constexpr int TOP_MARGIN = 4; // MUST match the island's
        constexpr int SIDE_MARGIN = 8;
        constexpr int SPACING = 6;     // between capsules
        constexpr int ISLAND_GAP = 12; // between the left cluster and the island's pill

        bool contains(const std::vector<int>& v, int n) { return std::find(v.begin(), v.end(), n) != v.end(); }

        const char* connector_of(GdkMonitor* monitor) {
            const char* c = gdk_monitor_get_connector(monitor);
            return c ? c : "";
        }

    } // namespace

    Bar::Bar(Compositor& compositor,
             Island& island,
             Audio& audio,
             Mpris& mpris,
             Power& power,
             Bluetooth& bluetooth,
             Network& network,
             TrayUi& tray)
        : compositor_(compositor)
        , island_(island)
        , audio_(audio)
        , mpris_(mpris)
        , power_(power)
        , bluetooth_(bluetooth)
        , network_(network)
        , tray_(tray) {}

    Bar::~Bar() {
        if (monitors_handler_)
            g_signal_handler_disconnect(gdk_display_get_monitors(gdk_display_get_default()), monitors_handler_);
        for (auto& [monitor, surface] : surfaces_)
            gtk_window_destroy(surface.window);
    }

    void Bar::start(GtkApplication* app) {
        app_ = app;
        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        monitors_handler_ = g_signal_connect(monitors, "items-changed", G_CALLBACK(on_monitors_changed), this);
        sync_monitors();
        mpris_.subscribe([this] {
            for (auto& [monitor, surface] : surfaces_)
                update_media(surface);
        });
        audio_.subscribe([this] {
            for (auto& [monitor, surface] : surfaces_)
                update_volume(surface);
        });
        power_.subscribe([this] {
            for (auto& [monitor, surface] : surfaces_)
                update_battery(surface);
        });
        bluetooth_.subscribe([this] {
            for (auto& [monitor, surface] : surfaces_)
                update_bluetooth(surface);
        });
        network_.subscribe([this] {
            for (auto& [monitor, surface] : surfaces_)
                update_network(surface);
        });
        island_.on_pill_resize([this] {
            for (auto& [monitor, surface] : surfaces_)
                gtk_widget_queue_allocate(surface.left);
        });
    }

    void Bar::on_monitors_changed(GListModel*, guint, guint, guint, gpointer data) {
        static_cast<Bar*>(data)->sync_monitors();
    }

    void Bar::sync_monitors() {
        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        const guint n = g_list_model_get_n_items(monitors);

        std::vector<GdkMonitor*> live;
        bool added = false;
        for (guint i = 0; i < n; i++) {
            // get_item returns a ref; the list keeps its own
            GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, i));
            g_object_unref(monitor);
            live.push_back(monitor);
            if (!surfaces_.contains(monitor)) {
                add_monitor(monitor);
                added = true;
            }
        }

        std::vector<GdkMonitor*> gone;
        for (auto& [monitor, surface] : surfaces_)
            if (std::find(live.begin(), live.end(), monitor) == live.end())
                gone.push_back(monitor);
        for (GdkMonitor* monitor : gone)
            drop_monitor(monitor);

        // A bar made after the island stacks above it on the same layer, so the island is re-created on top.
        GdkMonitor* keep = island_.monitor();
        const bool lost = !keep || std::find(live.begin(), live.end(), keep) == live.end();
        if ((added || lost) && !live.empty())
            island_.remap(lost ? live.front() : keep);
    }

    void Bar::add_monitor(GdkMonitor* monitor) {
        GtkWindow* window = GTK_WINDOW(gtk_application_window_new(app_));
        gtk_layer_init_for_window(window);
        gtk_layer_set_namespace(window, "fenriz-bar");
        gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_BOTTOM);
        gtk_layer_set_monitor(window, monitor);
        gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, TOP_MARGIN);
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, SIDE_MARGIN);
        gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, SIDE_MARGIN);
        gtk_layer_auto_exclusive_zone_enable(window);
        gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_widget_add_css_class(GTK_WIDGET(window), "fenriz-bar");

        GtkWidget* row = gtk_center_box_new();
        gtk_widget_add_css_class(row, "bar-row");
        GtkWidget* left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
        g_object_set_data(G_OBJECT(left), "bar", this);
        gtk_widget_set_layout_manager(left, gtk_custom_layout_new(nullptr, measure_left, allocate_left));
        GtkWidget* right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
        gtk_center_box_set_start_widget(GTK_CENTER_BOX(row), left);
        gtk_center_box_set_end_widget(GTK_CENTER_BOX(row), right);

        GtkWidget* workspaces = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(workspaces, "bar-capsule");
        gtk_widget_add_css_class(workspaces, "bar-workspaces");
        gtk_widget_set_visible(workspaces, FALSE);
        g_object_set_data(G_OBJECT(workspaces), "monitor", monitor);
        GtkEventController* scroll = gtk_event_controller_scroll_new(static_cast<GtkEventControllerScrollFlags>(
            GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
        g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), this);
        gtk_widget_add_controller(workspaces, scroll);
        gtk_box_append(GTK_BOX(left), workspaces);

        GtkWidget* window_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(window_box, "bar-capsule");
        gtk_widget_add_css_class(window_box, "bar-window");
        gtk_widget_set_visible(window_box, FALSE);
        GtkWidget* icon = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
        GtkWidget* title = gtk_label_new(nullptr);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(title, "bar-title");
        gtk_box_append(GTK_BOX(window_box), icon);
        gtk_box_append(GTK_BOX(window_box), title);
        gtk_box_append(GTK_BOX(left), window_box);

        GtkWidget* media_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* media_icon = gtk_image_new_from_icon_name("fenriz-play-symbolic");
        GtkWidget* media_label = gtk_label_new(nullptr);
        gtk_label_set_ellipsize(GTK_LABEL(media_label), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(media_box), media_icon);
        gtk_box_append(GTK_BOX(media_box), media_label);
        GtkWidget* media = page_button(monitor, "media", media_box, "bar-media");
        gtk_box_append(GTK_BOX(left), media);

        for (const auto& create : statuses_)
            gtk_box_append(GTK_BOX(right), create());
        gtk_box_append(GTK_BOX(right), tray_.create()); // app icons lead the status glyphs

        GtkWidget* volume = page_button(monitor, "audio", gtk_image_new(), "bar-glyph");
        GtkEventController* volume_scroll = gtk_event_controller_scroll_new(static_cast<GtkEventControllerScrollFlags>(
            GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
        g_signal_connect(volume_scroll, "scroll", G_CALLBACK(on_volume_scroll), this);
        gtk_widget_add_controller(volume, volume_scroll);
        gtk_box_append(GTK_BOX(right), volume);

        GtkWidget* bluetooth = page_button(monitor, "bluetooth", gtk_image_new(), "bar-glyph");
        gtk_box_append(GTK_BOX(right), bluetooth);

        GtkWidget* stats = page_button(
            monitor, "system", gtk_image_new_from_icon_name("fenriz-chip-symbolic"), "bar-glyph");
        gtk_box_append(GTK_BOX(right), stats);

        GtkWidget* network = page_button(monitor, "wifi", gtk_image_new(), "bar-glyph");
        gtk_box_append(GTK_BOX(right), network);

        GtkWidget* battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(battery_box, GTK_ALIGN_CENTER);
        GtkWidget* battery_icon = gtk_image_new();
        GtkWidget* battery_label = gtk_label_new(nullptr);
        gtk_widget_add_css_class(battery_label, "bar-battery");
        gtk_box_append(GTK_BOX(battery_box), battery_icon);
        gtk_box_append(GTK_BOX(battery_box), battery_label);
        GtkWidget* battery = page_button(monitor, "power", battery_box, "bar-glyph");
        GtkGesture* battery_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(battery_click), GDK_BUTTON_SECONDARY);
        g_signal_connect(battery_click,
                         "released",
                         G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
                             auto* self = static_cast<Bar*>(data);
                             self->battery_percent_ = !self->battery_percent_;
                             for (auto& [_, surface] : self->surfaces_)
                                 self->update_battery(surface);
                         }),
                         this);
        gtk_widget_add_controller(battery, GTK_EVENT_CONTROLLER(battery_click));
        gtk_box_append(GTK_BOX(right), battery);

        GtkWidget* shutdown =
            page_button(monitor, "power", gtk_image_new_from_icon_name("fenriz-power-symbolic"), "bar-glyph");
        gtk_box_append(GTK_BOX(right), shutdown);

        gtk_window_set_child(window, row);
        desktop::blur::attach(GTK_NATIVE(window));

        surfaces_[monitor] = Surface{.window = window,
                                     .left = left,
                                     .workspaces = workspaces,
                                     .window_box = window_box,
                                     .window_icon = icon,
                                     .window_title = title,
                                     .built = "",
                                     .media = media,
                                     .media_icon = media_icon,
                                     .media_label = media_label,
                                     .volume = volume,
                                     .bluetooth = bluetooth,
                                     .network = network,
                                     .battery = battery,
                                     .battery_icon = battery_icon,
                                     .battery_label = battery_label};
        update_surface(monitor, surfaces_[monitor], compositor_.state());
        update_media(surfaces_[monitor]);
        update_volume(surfaces_[monitor]);
        update_battery(surfaces_[monitor]);
        update_bluetooth(surfaces_[monitor]);
        update_network(surfaces_[monitor]);
        gtk_window_present(window);
    }

    void Bar::drop_monitor(GdkMonitor* monitor) {
        auto it = surfaces_.find(monitor);
        if (it == surfaces_.end())
            return;
        gtk_window_destroy(it->second.window);
        surfaces_.erase(it);
    }

    void Bar::measure_left(GtkWidget* left, GtkOrientation orientation, int, int* minimum, int* natural, int*, int*) {
        const bool horizontal = orientation == GTK_ORIENTATION_HORIZONTAL;
        int shown = 0;
        *minimum = *natural = 0;
        for (GtkWidget* c = gtk_widget_get_first_child(left); c; c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c))
                continue;
            int min = 0, nat = 0;
            gtk_widget_measure(c, orientation, -1, &min, &nat, nullptr, nullptr);
            if (horizontal) {
                *minimum += min;
                *natural += nat;
                shown++;
            } else {
                *minimum = std::max(*minimum, min);
                *natural = std::max(*natural, nat);
            }
        }
        if (shown > 1) {
            *minimum += (shown - 1) * SPACING;
            *natural += (shown - 1) * SPACING;
        }
    }

    // The row is centered on the monitor like the island, so the pill starts half its width left of the row's middle.
    void Bar::allocate_left(GtkWidget* left, int width, int height, int) {
        const auto* self = static_cast<const Bar*>(g_object_get_data(G_OBJECT(left), "bar"));
        const int clear =
            gtk_widget_get_width(gtk_widget_get_parent(left)) / 2 - self->island_.pill_width() / 2 - ISLAND_GAP;
        int budget = std::min(width, clear);

        struct Child {
            GtkWidget* widget;
            int min, nat;
        };
        std::vector<Child> fixed, flex;
        for (GtkWidget* c = gtk_widget_get_first_child(left); c; c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c))
                continue;
            Child child{c, 0, 0};
            gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &child.min, &child.nat, nullptr, nullptr);
            // the first child is the workspaces, which never shrink
            (c == gtk_widget_get_first_child(left) ? fixed : flex).push_back(child);
        }
        const size_t shown = fixed.size() + flex.size();
        if (shown > 1)
            budget -= static_cast<int>(shown - 1) * SPACING;
        for (const Child& c : fixed)
            budget -= c.nat;

        int x = 0;
        const auto place = [&](GtkWidget* widget, int w) {
            const GtkAllocation at{x, 0, w, height};
            gtk_widget_size_allocate(widget, &at, -1);
            x += w + SPACING;
        };
        for (const Child& c : fixed)
            place(c.widget, c.nat);
        // ponytail: a pair (title, song); a third flexible capsule would need a real n-way split
        for (size_t i = 0; i < flex.size(); i++) {
            const int other = flex.size() == 2 ? flex[1 - i].nat : 0;
            place(flex[i].widget, std::max(flex[i].min, flex_share(flex[i].nat, other, budget)));
        }
    }

    GdkMonitor* Bar::monitor_for(const std::string& connector) const {
        for (const auto& [monitor, surface] : surfaces_)
            if (connector == connector_of(monitor))
                return monitor;
        return nullptr;
    }

    void Bar::update(const CompositorState& state) {
        for (auto& [monitor, surface] : surfaces_)
            update_surface(monitor, surface, state);
    }

    void Bar::update_surface(GdkMonitor* monitor, Surface& surface, const CompositorState& state) {
        const int active = state.active_on(connector_of(monitor));

        // Rebuilt only when what they show changed; a title change must not recreate buttons under the pointer.
        std::string key = std::to_string(active) + "|";
        for (int n : state.occupied)
            key += std::to_string(n) + (contains(state.urgent, n) ? "!" : "") + ",";
        for (const auto& o : state.outputs)
            key += "|" + std::to_string(o.active);
        if (key != surface.built) {
            surface.built = key;
            while (GtkWidget* child = gtk_widget_get_first_child(surface.workspaces))
                gtk_box_remove(GTK_BOX(surface.workspaces), child);
            for (int n : state.occupied) {
                GtkWidget* button = gtk_button_new_with_label(std::to_string(n).c_str());
                gtk_widget_add_css_class(button, "workspace");
                if (n == active)
                    gtk_widget_add_css_class(button, "active");
                else if (std::any_of(
                             state.outputs.begin(), state.outputs.end(), [n](const auto& o) { return o.active == n; }))
                    gtk_widget_add_css_class(button, "visible"); // shown on another screen
                if (contains(state.urgent, n))
                    gtk_widget_add_css_class(button, "urgent");
                g_object_set_data(G_OBJECT(button), "workspace", GINT_TO_POINTER(n));
                g_signal_connect(button, "clicked", G_CALLBACK(on_workspace_clicked), this);
                gtk_box_append(GTK_BOX(surface.workspaces), button);
            }
            gtk_widget_set_visible(surface.workspaces, !state.occupied.empty());
        }

        gtk_widget_set_visible(surface.window_box, state.has_window && !state.title.empty());
        gtk_label_set_text(GTK_LABEL(surface.window_title), state.title.c_str());
        GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
        const std::string& icon = !state.icon.empty() ? state.icon : state.app_id;
        const bool has_icon = !icon.empty() && gtk_icon_theme_has_icon(theme, icon.c_str());
        gtk_widget_set_visible(surface.window_icon, has_icon);
        if (has_icon)
            gtk_image_set_from_icon_name(GTK_IMAGE(surface.window_icon), icon.c_str());
    }

    GtkWidget* Bar::page_button(GdkMonitor* monitor, const char* page, GtkWidget* child, const char* css_class) {
        GtkWidget* button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), child);
        gtk_widget_add_css_class(button, "bar-capsule");
        gtk_widget_add_css_class(button, css_class);
        g_object_set_data(G_OBJECT(button), "monitor", monitor);
        g_object_set_data(G_OBJECT(button), "page", const_cast<char*>(page));
        g_signal_connect(button,
                         "clicked",
                         G_CALLBACK(+[](GtkButton* b, gpointer data) {
                             static_cast<Island*>(data)->toggle(
                                 static_cast<const char*>(g_object_get_data(G_OBJECT(b), "page")),
                                 static_cast<GdkMonitor*>(g_object_get_data(G_OBJECT(b), "monitor")));
                         }),
                         &island_);
        return button;
    }

    void Bar::update_media(Surface& surface) {
        const Player* p = mpris_.active();
        gtk_widget_set_visible(surface.media, p && !p->track.title.empty());
        if (!p)
            return;
        const std::string text = p->track.artist.empty() ? p->track.title : p->track.artist + " – " + p->track.title;
        gtk_label_set_text(GTK_LABEL(surface.media_label), text.c_str());
        gtk_image_set_from_icon_name(GTK_IMAGE(surface.media_icon),
                                     p->playing() ? "fenriz-play-symbolic" : "fenriz-pause-symbolic");
    }

    void Bar::update_volume(Surface& surface) {
        const Audio::Endpoint& sink = audio_.sink();
        gtk_widget_set_visible(surface.volume, sink.id != 0);
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_child(GTK_BUTTON(surface.volume))),
                                     desktop::volume_icon(sink.percent, sink.muted));
    }

    void Bar::update_battery(Surface& surface) {
        const Power::Battery& b = power_.battery();
        gtk_widget_set_visible(surface.battery, b.present);
        if (!b.present)
            return;
        gtk_image_set_from_icon_name(GTK_IMAGE(surface.battery_icon), b.icon.c_str());
        gtk_widget_set_visible(surface.battery_label, battery_percent_);
        const std::string text = std::to_string(static_cast<int>(b.percent + 0.5)) + "%";
        gtk_label_set_text(GTK_LABEL(surface.battery_label), text.c_str());
    }

    void Bar::update_bluetooth(Surface& surface) {
        gtk_widget_set_visible(surface.bluetooth, bluetooth_.available());
        const bool connected = std::any_of(
            bluetooth_.devices().begin(), bluetooth_.devices().end(), [](const BtDevice& d) { return d.connected; });
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_child(GTK_BUTTON(surface.bluetooth))),
                                     !bluetooth_.powered() ? "fenriz-bluetooth-off-symbolic"
                                     : connected           ? "fenriz-bluetooth-connect-symbolic"
                                                           : "fenriz-bluetooth-symbolic");
    }

    void Bar::update_network(Surface& surface) {
        gtk_widget_set_visible(surface.network, network_.running() && (network_.has_wifi() || network_.wired()));
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_child(GTK_BUTTON(surface.network))), network_.icon());
    }

    gboolean Bar::on_volume_scroll(GtkEventControllerScroll*, double, double dy, gpointer data) {
        auto* self = static_cast<Bar*>(data);
        const Audio::Endpoint& sink = self->audio_.sink();
        if (sink.id == 0 || dy == 0)
            return FALSE;
        self->audio_.set_percent(false, sink.percent + (dy < 0 ? 5 : -5));
        return TRUE;
    }

    void Bar::on_workspace_clicked(GtkButton* button, gpointer data) {
        const int n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "workspace"));
        static_cast<Bar*>(data)->compositor_.workspace(n);
    }

    // Steps through the occupied workspaces from the one this screen shows.
    gboolean Bar::on_scroll(GtkEventControllerScroll* scroll, double, double dy, gpointer data) {
        auto* self = static_cast<Bar*>(data);
        GtkWidget* widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(scroll));
        auto* monitor = static_cast<GdkMonitor*>(g_object_get_data(G_OBJECT(widget), "monitor"));
        const CompositorState& state = self->compositor_.state();
        const auto& occ = state.occupied;
        auto it = std::find(occ.begin(), occ.end(), state.active_on(connector_of(monitor)));
        if (it == occ.end() || dy == 0)
            return FALSE;
        if (dy > 0 && it + 1 != occ.end())
            self->compositor_.workspace(*(it + 1));
        else if (dy < 0 && it != occ.begin())
            self->compositor_.workspace(*(it - 1));
        return TRUE;
    }

} // namespace fenriz::bar
