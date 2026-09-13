#include "island.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <tuple>

#include "blur.hpp"

namespace {

    constexpr int PILL_HEIGHT = 30;
    constexpr int PILL_PADDING = 16; // each side of the pill's content
    constexpr int HOVER_GROW_X = 8;  // total, split across both sides
    constexpr int HOVER_GROW_Y = 4;  // downwards only; the top edge stays on the bar
    constexpr double OPEN_RADIUS = 22;
    constexpr double MAX_BLUR = 8; // px of blur on content still fading in
    constexpr double MIN_SCALE = 0.96;

} // namespace

// The one widget on the island's surface. It always measures as the whole surface, and draws its children clipped
// to the animated rectangle, so nothing it contains is ever re-laid-out mid-morph.
struct FenrizIsland {
    GtkWidget parent;
    fenriz::bar::Island* island;
};

struct FenrizIslandClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(FenrizIsland, fenriz_island, GTK_TYPE_WIDGET)

static void fenriz_island_measure(GtkWidget* widget,
                                  GtkOrientation orientation,
                                  int,
                                  int* minimum,
                                  int* natural,
                                  int* min_baseline,
                                  int* nat_baseline) {
    reinterpret_cast<FenrizIsland*>(widget)->island->measure(orientation, minimum, natural);
    *min_baseline = *nat_baseline = -1;
}

static void fenriz_island_size_allocate(GtkWidget* widget, int width, int height, int) {
    reinterpret_cast<FenrizIsland*>(widget)->island->allocate(width, height);
}

static void fenriz_island_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    reinterpret_cast<FenrizIsland*>(widget)->island->snapshot(widget, snapshot);
}

static void fenriz_island_dispose(GObject* object) {
    reinterpret_cast<FenrizIsland*>(object)->island->dispose();
    G_OBJECT_CLASS(fenriz_island_parent_class)->dispose(object);
}

static void fenriz_island_class_init(FenrizIslandClass* klass) {
    GtkWidgetClass* widget = GTK_WIDGET_CLASS(klass);
    widget->measure = fenriz_island_measure;
    widget->size_allocate = fenriz_island_size_allocate;
    widget->snapshot = fenriz_island_snapshot;
    G_OBJECT_CLASS(klass)->dispose = fenriz_island_dispose;
}

static void fenriz_island_init(FenrizIsland*) {}

namespace fenriz::bar {

    namespace {

        double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

        GtkWidget* label(const char* css_class) {
            GtkWidget* l = gtk_label_new(nullptr);
            gtk_widget_add_css_class(l, css_class);
            return l;
        }

    } // namespace

    Island::~Island() {
        for (guint id : {clock_id_, activity_id_, remeasure_id_})
            if (id)
                g_source_remove(id);
        if (window_)
            gtk_window_destroy(window_);
    }

    bool Island::has_page(const std::string& page) const {
        return gtk_stack_get_child_by_name(GTK_STACK(stack_), page.c_str()) != nullptr;
    }

    void Island::add_page(const std::string& name, GtkWidget* page) {
        gtk_widget_add_css_class(page, "island-page");
        gtk_stack_add_named(GTK_STACK(stack_), page, name.c_str());
    }

    void Island::add_to_home(GtkWidget* widget) { gtk_box_append(GTK_BOX(home_box_), widget); }

    void Island::add_tile(GtkWidget* tile) {
        gtk_box_append(GTK_BOX(tiles_), tile);
        gtk_widget_set_visible(tiles_, TRUE);
    }

    void Island::add_to_footer(GtkWidget* widget) {
        gtk_box_append(GTK_BOX(footer_), widget);
        gtk_widget_set_visible(footer_, TRUE);
    }

    void Island::navigate(const std::string& page) { show_page(page, true); }

    GtkWidget* Island::page_header(const char* title) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* back = gtk_button_new_from_icon_name("go-previous-symbolic");
        gtk_widget_add_css_class(back, "island-back");
        g_signal_connect_swapped(back, "clicked", G_CALLBACK(+[](Island* self) { self->navigate("home"); }), this);
        gtk_box_append(GTK_BOX(row), back);
        GtkWidget* label = gtk_label_new(title);
        gtk_widget_add_css_class(label, "island-title");
        gtk_box_append(GTK_BOX(row), label);
        return row;
    }

    void Island::start(GtkApplication* app) {
        app_ = app;
        window_ = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, "fenriz-island");
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_margin(window_, GTK_LAYER_SHELL_EDGE_TOP, 4); // MUST match the bar's
        gtk_layer_set_exclusive_zone(window_, -1);                  // sit on the bar, not below its reserved space
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_widget_add_css_class(GTK_WIDGET(window_), "fenriz-island");

        root_ = GTK_WIDGET(g_object_new(fenriz_island_get_type(), nullptr));
        reinterpret_cast<FenrizIsland*>(root_)->island = this;

        shape_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(shape_, "island-shape");
        gtk_widget_set_parent(shape_, root_);

        pill_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(pill_, "island-pill");
        pill_stack_ = gtk_stack_new();
        gtk_stack_set_hhomogeneous(GTK_STACK(pill_stack_), FALSE);
        gtk_stack_set_transition_type(GTK_STACK(pill_stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(pill_stack_), 150);
        gtk_box_append(GTK_BOX(pill_), pill_stack_);

        pill_label_ = label("island-clock");
        gtk_stack_add_named(GTK_STACK(pill_stack_), pill_label_, "clock");

        GtkWidget* osd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        osd_icon_ = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(osd_icon_), 16);
        osd_level_ = gtk_level_bar_new_for_interval(0, 100);
        gtk_level_bar_set_mode(GTK_LEVEL_BAR(osd_level_), GTK_LEVEL_BAR_MODE_CONTINUOUS);
        gtk_widget_add_css_class(osd_level_, "island-level");
        gtk_widget_set_valign(osd_level_, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(osd), osd_icon_);
        gtk_box_append(GTK_BOX(osd), osd_level_);
        gtk_stack_add_named(GTK_STACK(pill_stack_), osd, "osd");

        GtkWidget* media = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        media_art_ = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(media_art_), 20);
        gtk_widget_set_valign(media_art_, GTK_ALIGN_CENTER);
        gtk_widget_set_overflow(media_art_, GTK_OVERFLOW_HIDDEN);
        gtk_widget_add_css_class(media_art_, "island-art");
        media_label_ = label("island-media");
        gtk_label_set_ellipsize(GTK_LABEL(media_label_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(media_label_), 36);
        gtk_box_append(GTK_BOX(media), media_art_);
        gtk_box_append(GTK_BOX(media), media_label_);
        gtk_stack_add_named(GTK_STACK(pill_stack_), media, "media");

        for (auto [name, icon, text] :
             {std::tuple{"event", &event_icon_, &event_label_}, std::tuple{"alert", &alert_icon_, &alert_label_}}) {
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            *icon = gtk_image_new();
            gtk_image_set_pixel_size(GTK_IMAGE(*icon), 16);
            *text = label("island-media");
            gtk_label_set_ellipsize(GTK_LABEL(*text), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(*text), 40);
            gtk_box_append(GTK_BOX(box), *icon);
            gtk_box_append(GTK_BOX(box), *text);
            gtk_widget_add_css_class(box, name);
            gtk_stack_add_named(GTK_STACK(pill_stack_), box, name);
        }
        gtk_stack_set_visible_child_name(GTK_STACK(pill_stack_), "clock");
        gtk_widget_set_parent(pill_, root_);

        build_pages();
        gtk_widget_set_parent(stack_, root_);
        gtk_widget_set_can_target(stack_, FALSE);

        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), this);
        gtk_widget_add_controller(root_, GTK_EVENT_CONTROLLER(click));

        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "enter", G_CALLBACK(on_enter), this);
        g_signal_connect(motion, "motion", G_CALLBACK(on_motion), this);
        g_signal_connect(motion, "leave", G_CALLBACK(on_leave), this);
        gtk_widget_add_controller(root_, motion);

        GtkEventController* key = gtk_event_controller_key_new();
        g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), this);
        gtk_widget_add_controller(GTK_WIDGET(window_), key);
        g_signal_connect(window_, "notify::is-active", G_CALLBACK(on_active), this);

        gtk_window_set_child(window_, root_);
        desktop::blur::attach(GTK_NATIVE(window_));

        update_clock();
        width_.snap();
        height_.snap();
        resize_surface(pill_width_ + HOVER_GROW_X, PILL_HEIGHT + HOVER_GROW_Y);
    }

    void Island::build_pages() {
        stack_ = gtk_stack_new();
        gtk_stack_set_hhomogeneous(GTK_STACK(stack_), FALSE);
        gtk_stack_set_vhomogeneous(GTK_STACK(stack_), FALSE);
        gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(stack_), 180);

        GtkWidget* home = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(home, "island-page");
        GtkWidget* header = gtk_button_new();
        gtk_widget_add_css_class(header, "island-header");
        GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        home_time_ = label("island-time");
        home_date_ = label("island-date");
        gtk_box_append(GTK_BOX(header_box), home_time_);
        gtk_box_append(GTK_BOX(header_box), home_date_);
        gtk_button_set_child(GTK_BUTTON(header), header_box);
        g_signal_connect_swapped(
            header, "clicked", G_CALLBACK(+[](Island* self) { self->show_page("calendar", true); }), this);
        gtk_box_append(GTK_BOX(home), header);
        tiles_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_set_homogeneous(GTK_BOX(tiles_), TRUE);
        gtk_widget_add_css_class(tiles_, "island-tiles");
        gtk_widget_set_visible(tiles_, FALSE);
        gtk_box_append(GTK_BOX(home), tiles_);
        home_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_add_css_class(home_box_, "island-controls");
        gtk_box_append(GTK_BOX(home), home_box_);
        footer_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(footer_, "island-footer");
        gtk_widget_set_visible(footer_, FALSE);
        gtk_box_append(GTK_BOX(home), footer_);
        gtk_stack_add_named(GTK_STACK(stack_), home, "home");

        GtkWidget* calendar_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(calendar_page), page_header("Calendar"));
        calendar_ = gtk_calendar_new();
        gtk_box_append(GTK_BOX(calendar_page), calendar_);
        add_page("calendar", calendar_page);
    }

    void Island::measure(GtkOrientation orientation, int* minimum, int* natural) const {
        *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? surface_width_ : surface_height_;
    }

    void Island::allocate(int width, int height) {
        gtk_widget_allocate(shape_, width, height, -1, nullptr);

        int pill_w = 0, pill_h = 0;
        gtk_widget_measure(pill_, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &pill_w, nullptr, nullptr);
        gtk_widget_measure(pill_, GTK_ORIENTATION_VERTICAL, pill_w, nullptr, &pill_h, nullptr, nullptr);
        graphene_point_t offset = GRAPHENE_POINT_INIT(static_cast<float>((width - pill_w) / 2),
                                                      static_cast<float>((PILL_HEIGHT - pill_h) / 2));
        gtk_widget_allocate(pill_, pill_w, pill_h, -1, gsk_transform_translate(nullptr, &offset));

        // Content changed size (a new clock minute, a device list, a player appearing): morph to fit, but from an idle,
        // since retargeting can resize the surface and that must not happen inside an allocation.
        bool changed = pill_w + 2 * PILL_PADDING != pill_width_;
        if (expanded_) {
            GtkWidget* child = gtk_stack_get_visible_child(GTK_STACK(stack_));
            int w = 0, h = 0;
            gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &w, nullptr, nullptr);
            gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, w, nullptr, &h, nullptr, nullptr);
            changed |= w != page_width_ || h != page_height_;
        }
        if (changed && !remeasure_id_)
            remeasure_id_ = g_idle_add(on_remeasure, this);

        // Laid out at the page's own size even before the surface has grown to fit it; the clip hides the overhang.
        offset = GRAPHENE_POINT_INIT(static_cast<float>((width - page_width_) / 2), 0);
        GskTransform* at = gsk_transform_translate(nullptr, &offset);
        gtk_widget_allocate(stack_, std::max(page_width_, 1), std::max(page_height_, 1), -1, at);
    }

    // 0 while the pill, 1 once the page has fully opened, read off the height so content follows the morph.
    double Island::open_progress() const {
        const double span = page_height_ - PILL_HEIGHT;
        if (span <= 0)
            return expanded_ ? 1 : 0;
        return clamp01((height_.value - PILL_HEIGHT) / span);
    }

    void Island::snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
        const double surface_w = gtk_widget_get_width(widget);
        const double w = width_.value, h = height_.value;
        const graphene_rect_t rect = GRAPHENE_RECT_INIT(
            static_cast<float>((surface_w - w) / 2), 0, static_cast<float>(w), static_cast<float>(h));
        GskRoundedRect clip;
        gsk_rounded_rect_init_from_rect(&clip, &rect, static_cast<float>(std::min(h / 2, OPEN_RADIUS)));
        gtk_snapshot_push_rounded_clip(snapshot, &clip);
        gtk_widget_snapshot_child(widget, shape_, snapshot);

        const double p = open_progress();
        const double pill_alpha = clamp01(1 - p * 3);
        if (pill_alpha > 0.01) {
            gtk_snapshot_push_opacity(snapshot, pill_alpha);
            gtk_widget_snapshot_child(widget, pill_, snapshot);
            gtk_snapshot_pop(snapshot);
        }

        const double content_alpha = clamp01((p - 0.25) / 0.75);
        if (content_alpha > 0.01) {
            const bool blurred = content_alpha < 0.99;
            const auto scale = static_cast<float>(MIN_SCALE + (1 - MIN_SCALE) * content_alpha);
            gtk_snapshot_push_opacity(snapshot, content_alpha);
            if (blurred)
                gtk_snapshot_push_blur(snapshot, (1 - content_alpha) * MAX_BLUR);
            gtk_snapshot_save(snapshot);
            // scale about the top-center, where the island hangs from
            const graphene_point_t pivot = GRAPHENE_POINT_INIT(static_cast<float>(surface_w / 2), 0);
            const graphene_point_t back = GRAPHENE_POINT_INIT(-pivot.x, 0);
            gtk_snapshot_translate(snapshot, &pivot);
            gtk_snapshot_scale(snapshot, scale, scale);
            gtk_snapshot_translate(snapshot, &back);
            gtk_widget_snapshot_child(widget, stack_, snapshot);
            gtk_snapshot_restore(snapshot);
            if (blurred)
                gtk_snapshot_pop(snapshot);
            gtk_snapshot_pop(snapshot);
        }
        gtk_snapshot_pop(snapshot);

        // Here rather than in the tick: GTK has finished laying the window out by now, so nothing resets it after.
        update_input_region();
    }

    void Island::dispose() {
        if (tick_id_) {
            gtk_widget_remove_tick_callback(root_, tick_id_);
            tick_id_ = 0;
        }
        g_clear_pointer(&shape_, gtk_widget_unparent);
        g_clear_pointer(&pill_, gtk_widget_unparent);
        g_clear_pointer(&stack_, gtk_widget_unparent);
    }

    // Clicks outside the drawn shape fall through to whatever is below.
    void Island::update_input_region() {
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
        if (!surface)
            return;
        const int w = static_cast<int>(width_.value + 0.5), h = static_cast<int>(height_.value + 0.5);
        const cairo_rectangle_int_t r = {(gtk_widget_get_width(root_) - w) / 2, 0, w, h};
        if (r.x == input_.x && r.y == input_.y && r.width == input_.width && r.height == input_.height)
            return;
        input_ = r;
        cairo_region_t* region = cairo_region_create_rectangle(&r);
        gdk_surface_set_input_region(surface, region);
        cairo_region_destroy(region);
    }

    void Island::grow_surface(int width, int height) {
        if (width > surface_width_ || height > surface_height_)
            resize_surface(std::max(surface_width_, width), std::max(surface_height_, height));
    }

    void Island::resize_surface(int width, int height) {
        surface_width_ = width;
        surface_height_ = height;
        // A window never shrinks to smaller content on its own; the default size is what lets it.
        gtk_window_set_default_size(window_, width, height);
        gtk_widget_queue_resize(root_);
    }

    void Island::set_target(double width, double height) {
        width_.target = width;
        height_.target = height;
        grow_surface(static_cast<int>(width) + HOVER_GROW_X, static_cast<int>(height) + HOVER_GROW_Y);
        kick();
    }

    void Island::retarget() {
        if (expanded_) {
            set_target(page_width_, page_height_);
            return;
        }
        int pill_w = 0;
        gtk_widget_measure(pill_, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &pill_w, nullptr, nullptr);
        if (pill_w + 2 * PILL_PADDING != pill_width_) {
            pill_width_ = pill_w + 2 * PILL_PADDING;
            for (auto& listener : pill_listeners_)
                listener();
        }
        double w = pill_width_, h = PILL_HEIGHT;
        if (hovered_) {
            w += HOVER_GROW_X;
            h += HOVER_GROW_Y;
        }
        set_target(w, h);
    }

    void Island::kick() {
        if (tick_id_ || !root_)
            return;
        last_frame_us_ = 0;
        tick_id_ = gtk_widget_add_tick_callback(root_, on_tick, this, nullptr);
    }

    gboolean Island::on_tick(GtkWidget*, GdkFrameClock* clock, gpointer data) {
        auto* self = static_cast<Island*>(data);
        const gint64 now = gdk_frame_clock_get_frame_time(clock);
        const double dt = self->last_frame_us_ ? (now - self->last_frame_us_) / 1e6 : 1.0 / 60;
        self->last_frame_us_ = now;

        self->width_.step(dt);
        self->height_.step(dt);
        gtk_widget_queue_draw(self->root_);
        if (!self->width_.settled() || !self->height_.settled())
            return G_SOURCE_CONTINUE;

        self->width_.snap();
        self->height_.snap();
        self->tick_id_ = 0;

        // Settled: give back the room the morph needed, in one resize. A collapsed surface keeps the hover slack, so
        // hovering never resizes it.
        const int want_w = (self->expanded_ ? self->page_width_ : self->pill_width_) + HOVER_GROW_X;
        const int want_h = (self->expanded_ ? self->page_height_ : PILL_HEIGHT) + HOVER_GROW_Y;
        if (want_w != self->surface_width_ || want_h != self->surface_height_)
            self->resize_surface(want_w, want_h);
        if (!self->expanded_ && self->page_ != "home") {
            gtk_stack_set_visible_child_full(GTK_STACK(self->stack_), "home", GTK_STACK_TRANSITION_TYPE_NONE);
            self->page_ = "home";
        }
        return G_SOURCE_REMOVE;
    }

    void Island::show_page(const std::string& page, bool animate) {
        if (page == "calendar") {
            GDateTime* now = g_date_time_new_now_local();
            gtk_calendar_set_date(GTK_CALENDAR(calendar_), now);
            g_date_time_unref(now);
        }
        page_ = page;
        armed_ = false; // the shape is about to change under the pointer
        gtk_stack_set_visible_child_full(GTK_STACK(stack_),
                                         page.c_str(),
                                         animate ? GTK_STACK_TRANSITION_TYPE_CROSSFADE
                                                 : GTK_STACK_TRANSITION_TYPE_NONE);
        measure_page();
        retarget();
    }

    void Island::measure_page() {
        GtkWidget* child = gtk_stack_get_child_by_name(GTK_STACK(stack_), page_.c_str());
        gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &page_width_, nullptr, nullptr);
        gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, page_width_, nullptr, &page_height_, nullptr, nullptr);
        gtk_widget_queue_allocate(root_);
    }

    gboolean Island::on_remeasure(gpointer data) {
        auto* self = static_cast<Island*>(data);
        self->remeasure_id_ = 0;
        if (self->expanded_)
            self->measure_page();
        self->retarget();
        return G_SOURCE_REMOVE;
    }

    void Island::set_activity(const Activity& activity, guint ms) {
        if (!activities_.push(activity))
            return;
        gtk_stack_set_visible_child_name(GTK_STACK(pill_stack_), activity.child.c_str());
        if (activity.sticky)
            return;
        if (activity_id_)
            g_source_remove(activity_id_);
        activity_id_ = g_timeout_add(ms, on_activity_done, this);
    }

    gboolean Island::on_activity_done(gpointer data) {
        auto* self = static_cast<Island*>(data);
        self->activity_id_ = 0;
        gtk_stack_set_visible_child_name(GTK_STACK(self->pill_stack_), self->activities_.expire().c_str());
        return G_SOURCE_REMOVE;
    }

    void Island::show_osd(const char* icon, int percent) {
        gtk_image_set_from_icon_name(GTK_IMAGE(osd_icon_), icon);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(osd_level_), std::clamp(percent, 0, 100));
        set_activity({"osd", ActivityQueue::LEVEL}, 1500);
    }

    void Island::show_media(const std::string& text, GdkPaintable* art) {
        if (activities_.current_priority() > ActivityQueue::MEDIA)
            return; // the pill's media page is not replaced while something else holds it
        gtk_label_set_text(GTK_LABEL(media_label_), text.c_str());
        set_media_art(art);
        set_activity({"media", ActivityQueue::MEDIA}, 3000);
    }

    void Island::show_event(const char* icon, const std::string& text) {
        if (activities_.current_priority() > ActivityQueue::EVENT)
            return;
        gtk_image_set_from_icon_name(GTK_IMAGE(event_icon_), icon);
        gtk_label_set_text(GTK_LABEL(event_label_), text.c_str());
        set_activity({"event", ActivityQueue::EVENT}, 3000);
    }

    void Island::show_alert(const char* icon, const std::string& text) {
        gtk_image_set_from_icon_name(GTK_IMAGE(alert_icon_), icon);
        gtk_label_set_text(GTK_LABEL(alert_label_), text.c_str());
        set_activity({"alert", ActivityQueue::EVENT, true}, 0);
    }

    void Island::dismiss_alert() {
        activities_.dismiss_sticky();
        gtk_stack_set_visible_child_name(GTK_STACK(pill_stack_), activities_.showing().c_str());
    }

    void Island::set_media_art(GdkPaintable* art) {
        gtk_image_set_from_paintable(GTK_IMAGE(media_art_), art);
        gtk_widget_set_visible(media_art_, art != nullptr);
    }

    void Island::toggle(const std::string& page, GdkMonitor* monitor) {
        if (expanded_ && page_ == page && (!monitor || monitor == monitor_))
            close();
        else
            open(page, monitor);
    }

    void Island::open(const std::string& page, GdkMonitor* monitor) {
        if (monitor && monitor != monitor_)
            remap(monitor);
        const bool was_open = expanded_;
        expanded_ = true;
        if (!was_open) {
            dismiss_alert(); // seen
            for (auto& listener : open_listeners_)
                listener();
        }
        had_focus_ = false;
        gtk_widget_set_can_target(stack_, TRUE);
        // An already-mapped surface is handed the keyboard by fenriz when its interactivity changes.
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        show_page(page, was_open);
        gtk_window_set_focus(window_, nullptr); // no focus ring until the user actually tabs
    }

    void Island::close() {
        if (!expanded_)
            return;
        expanded_ = false;
        hovered_ = false;
        armed_ = false;
        for (auto& listener : close_listeners_)
            listener();
        gtk_widget_set_can_target(stack_, FALSE);
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        retarget();
    }

    void Island::remap(GdkMonitor* monitor) {
        monitor_ = monitor;
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
        gtk_layer_set_monitor(window_, monitor);
        input_ = {};
        gtk_window_present(window_);
    }

    void Island::update_clock() {
        GDateTime* now = g_date_time_new_now_local();
        char* pill = g_date_time_format(now, "%a %-d  · %-l:%M");
        char* time = g_date_time_format(now, "%-l:%M");
        char* date = g_date_time_format(now, "%A, %B %-d");
        gtk_label_set_text(GTK_LABEL(pill_label_), pill);
        gtk_label_set_text(GTK_LABEL(home_time_), time);
        gtk_label_set_text(GTK_LABEL(home_date_), date);
        g_free(pill);
        g_free(time);
        g_free(date);

        // re-armed each minute, on the minute
        const guint ms = (60 - g_date_time_get_second(now)) * 1000 - g_date_time_get_microsecond(now) / 1000;
        g_date_time_unref(now);
        clock_id_ = g_timeout_add(ms + 50, on_clock, this);
        retarget();
    }

    gboolean Island::on_clock(gpointer data) {
        auto* self = static_cast<Island*>(data);
        self->clock_id_ = 0;
        self->update_clock();
        return G_SOURCE_REMOVE;
    }

    void Island::on_pressed(GtkGestureClick*, int, double, double, gpointer data) {
        auto* self = static_cast<Island*>(data);
        if (!self->expanded_)
            self->open("home", nullptr);
    }

    void Island::on_enter(GtkEventControllerMotion*, double, double, gpointer data) {
        auto* self = static_cast<Island*>(data);
        self->hovered_ = true;
        if (!self->expanded_)
            self->retarget();
    }

    // Only a pointer that has been inside the settled page closes it by leaving, so one opened from elsewhere, or
    // shrunk out from under the pointer, stays open.
    void Island::on_motion(GtkEventControllerMotion*, double, double, gpointer data) {
        auto* self = static_cast<Island*>(data);
        if (self->expanded_ && !self->tick_id_)
            self->armed_ = true;
    }

    void Island::on_leave(GtkEventControllerMotion*, gpointer data) {
        auto* self = static_cast<Island*>(data);
        self->hovered_ = false;
        if (!self->expanded_)
            self->retarget();
        else if (self->armed_)
            self->close();
    }

    gboolean Island::on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
        auto* self = static_cast<Island*>(data);
        if (!self->expanded_)
            return FALSE;
        if (keyval == GDK_KEY_Escape) {
            self->close();
            return TRUE;
        }
        GtkWidget* focus = gtk_window_get_focus(self->window_);
        if (keyval == GDK_KEY_BackSpace && self->page_ != "home" && !(focus && GTK_IS_EDITABLE(focus))) {
            self->show_page("home", true);
            return TRUE;
        }
        return FALSE;
    }

    // Losing the keyboard means the user went elsewhere, which closes the island like a popover.
    void Island::on_active(GObject*, GParamSpec*, gpointer data) {
        auto* self = static_cast<Island*>(data);
        if (gtk_window_is_active(self->window_))
            self->had_focus_ = true;
        else if (self->expanded_ && self->had_focus_)
            self->close();
    }

} // namespace fenriz::bar
