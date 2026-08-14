#include "wallpaper_picker.hpp"

#include <gtk4-layer-shell.h>

#include <filesystem>

#include "blur.hpp"
#include "spawn.hpp"
#include "wallpaper.hpp"

namespace fenriz::desktop {

    namespace {
        constexpr int COLUMNS = 3;
        constexpr int WIDTH = 1080;
        constexpr int HEIGHT = 700;
        constexpr int TILE_WIDTH = wallpaper::THUMB_WIDTH;
        constexpr int TILE_HEIGHT = wallpaper::THUMB_HEIGHT;
    } // namespace

    WallpaperPicker::WallpaperPicker(Config& cfg, Background& background) : cfg_(cfg), background_(background) {}

    WallpaperPicker::~WallpaperPicker() {
        if (thumbnail_source_)
            g_source_remove(thumbnail_source_);
        if (place_source_)
            g_source_remove(place_source_);
        if (window_)
            gtk_window_destroy(window_);
    }

    // Each population gets a brand-new GtkFlowBox rather than reusing one via gtk_flow_box_remove_all().
    void WallpaperPicker::load() {
        // A generation still in flight indexes the old vectors, so stop it before rebuilding.
        if (thumbnail_source_) {
            g_source_remove(thumbnail_source_);
            thumbnail_source_ = 0;
        }
        // pending placement points at the grid this call is about to replace
        if (place_source_) {
            g_source_remove(place_source_);
            place_source_ = 0;
        }
        pictures_.clear();
        current_index_ = -1;
        next_ = 0;

        grid_ = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid_), GTK_SELECTION_SINGLE);
        gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(grid_), TRUE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid_), TRUE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid_), COLUMNS);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid_), 10);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid_), 10);
        gtk_flow_box_set_filter_func(GTK_FLOW_BOX(grid_), on_filter, this, nullptr);
        gtk_widget_set_margin_start(grid_, 10);
        gtk_widget_set_margin_end(grid_, 10);
        gtk_widget_set_margin_bottom(grid_, 10);
        g_signal_connect(grid_, "child-activated", G_CALLBACK(on_child_activated), this);
        g_signal_connect(grid_, "map", G_CALLBACK(on_grid_map), this);

        paths_ = wallpaper::scan(cfg_.wallpaper_dir);
        if (paths_.empty())
            g_warning("wallpaper: no images under %s", cfg_.wallpaper_dir.c_str());

        const std::string& current = cfg_.wallpaper_for("");
        for (size_t i = 0; i < paths_.size(); i++) {
            const std::string& path = paths_[i];
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(box, "wallpaper-tile");

            GtkWidget* picture = gtk_picture_new();
            gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
            gtk_widget_set_size_request(picture, TILE_WIDTH, TILE_HEIGHT);
            gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(picture, GTK_ALIGN_CENTER);
            // lets the css border-radius actually clip the thumbnail
            gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
            gtk_box_append(GTK_BOX(box), picture);
            pictures_.push_back(picture);

            if (cfg_.wallpaper_search) {
                GtkWidget* label = gtk_label_new(std::filesystem::path(path).filename().string().c_str());
                gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
                // Together these ellipsize a long filename to the tile instead of widening it.
                gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
                gtk_widget_set_size_request(label, TILE_WIDTH, -1);
                gtk_box_append(GTK_BOX(box), label);
            }

            GtkWidget* child = gtk_flow_box_child_new();
            gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), box);
            g_object_set_data_full(G_OBJECT(child), "path", g_strdup(path.c_str()), g_free);
            gtk_flow_box_append(GTK_FLOW_BOX(grid_), child);

            if (path == current)
                current_index_ = static_cast<int>(i);
        }

        // Drops the previous box, and the stale cursor pointer inside it, with it.
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), grid_);

        if (!paths_.empty())
            thumbnail_source_ = g_idle_add(on_thumbnail, this);
    }

    // One image per main-loop iteration, so a cold cache doesn't freeze the ui.
    gboolean WallpaperPicker::on_thumbnail(gpointer data) {
        auto* self = static_cast<WallpaperPicker*>(data);
        if (self->next_ >= self->paths_.size()) {
            self->thumbnail_source_ = 0;
            return G_SOURCE_REMOVE;
        }

        const size_t i = self->next_++;
        const std::string thumb = wallpaper::ensure_thumbnail(self->paths_[i]);
        if (!thumb.empty()) {
            GError* err = nullptr;
            GdkTexture* texture = gdk_texture_new_from_filename(thumb.c_str(), &err);
            if (texture) {
                gtk_picture_set_paintable(GTK_PICTURE(self->pictures_[i]), GDK_PAINTABLE(texture));
                g_object_unref(texture);
            } else {
                g_warning("wallpaper: %s: %s", thumb.c_str(), err->message);
                g_error_free(err);
            }
        }
        return G_SOURCE_CONTINUE;
    }

    bool WallpaperPicker::matches(GtkFlowBoxChild* child) const {
        if (!search_)
            return true;
        const char* raw = gtk_editable_get_text(GTK_EDITABLE(search_));
        if (!raw || !*raw)
            return true;
        const auto* path = static_cast<const char*>(g_object_get_data(G_OBJECT(child), "path"));
        return path && g_str_match_string(raw, path, TRUE);
    }

    gboolean WallpaperPicker::on_filter(GtkFlowBoxChild* child, gpointer data) {
        return static_cast<WallpaperPicker*>(data)->matches(child);
    }

    void WallpaperPicker::on_search_changed(GtkEditable*, gpointer data) {
        auto* self = static_cast<WallpaperPicker*>(data);
        if (!self->grid_)
            return;
        gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(self->grid_));
        if (GtkFlowBoxChild* first = self->first_match())
            gtk_flow_box_select_child(GTK_FLOW_BOX(self->grid_), first);
    }

    GtkFlowBoxChild* WallpaperPicker::first_match() const {
        if (!grid_)
            return nullptr;
        for (GtkWidget* child = gtk_widget_get_first_child(grid_); child; child = gtk_widget_get_next_sibling(child))
            if (matches(GTK_FLOW_BOX_CHILD(child)))
                return GTK_FLOW_BOX_CHILD(child);
        return nullptr;
    }

    // What Enter acts on: the selection while it still matches, otherwise the top match.
    GtkFlowBoxChild* WallpaperPicker::current() const {
        if (!grid_)
            return nullptr;
        GList* selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(grid_));
        GtkFlowBoxChild* child = selected ? GTK_FLOW_BOX_CHILD(selected->data) : nullptr;
        g_list_free(selected);
        if (child && matches(child))
            return child;
        return first_match();
    }

    void WallpaperPicker::activate(GtkFlowBoxChild* child) {
        if (!child)
            return;
        const auto* path = static_cast<const char*>(g_object_get_data(G_OBJECT(child), "path"));
        if (!path)
            return;

        cfg_.selected_wallpaper = path;
        save_selected_wallpaper(cfg_.selected_wallpaper);
        background_.reload();
        if (!cfg_.wallpaper_hook.empty())
            spawn::hook(cfg_.wallpaper_hook, cfg_.selected_wallpaper);
        close();
    }

    void WallpaperPicker::on_child_activated(GtkFlowBox*, GtkFlowBoxChild* child, gpointer data) {
        static_cast<WallpaperPicker*>(data)->activate(child);
    }

    gboolean WallpaperPicker::on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
        auto* self = static_cast<WallpaperPicker*>(data);
        const bool pending_g = self->pending_g_;
        self->pending_g_ = false;

        switch (keyval) {
        case GDK_KEY_Escape:
            self->close();
            return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            self->activate(self->current());
            return TRUE;
        default:
            break;
        }

        // upgrade to vim mode
        if (self->search_)
            return FALSE;

        switch (keyval) {
        case GDK_KEY_h:
            self->move(GTK_MOVEMENT_VISUAL_POSITIONS, -1);
            return TRUE;
        case GDK_KEY_l:
            self->move(GTK_MOVEMENT_VISUAL_POSITIONS, 1);
            return TRUE;
        case GDK_KEY_j:
            self->move(GTK_MOVEMENT_DISPLAY_LINES, 1);
            return TRUE;
        case GDK_KEY_k:
            self->move(GTK_MOVEMENT_DISPLAY_LINES, -1);
            return TRUE;
        case GDK_KEY_g:
            if (pending_g)
                self->move(GTK_MOVEMENT_BUFFER_ENDS, -1);
            else
                self->pending_g_ = true;
            return TRUE;
        case GDK_KEY_G:
            self->move(GTK_MOVEMENT_BUFFER_ENDS, 1);
            return TRUE;
        case GDK_KEY_space:
            self->activate(self->current());
            return TRUE;
        case GDK_KEY_q:
            self->close();
            return TRUE;
        default:
            return FALSE;
        }
    }

    void WallpaperPicker::build(GtkApplication* app) {
        window_ = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, "fenriz-wallpaper");
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_window_set_default_size(window_, WIDTH, HEIGHT);
        gtk_widget_add_css_class(GTK_WIDGET(window_), "fenriz-shell");

        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(root, "fenriz-wallpaper");

        if (cfg_.wallpaper_search) {
            search_ = gtk_search_entry_new();
            gtk_widget_add_css_class(search_, "fenriz-field");
            gtk_widget_set_margin_start(search_, 10);
            gtk_widget_set_margin_end(search_, 10);
            gtk_widget_set_margin_top(search_, 10);
            gtk_widget_set_margin_bottom(search_, 6);
            g_signal_connect(search_, "changed", G_CALLBACK(on_search_changed), this);
            gtk_box_append(GTK_BOX(root), search_);
        } else {
            gtk_widget_add_css_class(root, "no-search");
        }

        scroll_ = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
        gtk_widget_set_vexpand(scroll_, TRUE);
        gtk_box_append(GTK_BOX(root), scroll_);

        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), this);
        gtk_widget_add_controller(GTK_WIDGET(window_), keys);

        gtk_window_set_child(window_, root);
        if (search_)
            gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(search_), GTK_WIDGET(window_));
        blur::attach(GTK_NATIVE(window_));
    }

    void WallpaperPicker::toggle(GtkApplication* app) {
        if (window_ && gtk_widget_get_visible(GTK_WIDGET(window_))) {
            close();
            return;
        }
        if (!window_)
            build(app);
        if (search_)
            gtk_editable_set_text(GTK_EDITABLE(search_), "");
        load();
        gtk_window_present(window_);
    }

    void WallpaperPicker::on_grid_map(GtkWidget*, gpointer data) {
        auto* self = static_cast<WallpaperPicker*>(data);
        if (!self->place_source_)
            self->place_source_ = g_idle_add(on_place_cursor, self);
    }

    gboolean WallpaperPicker::on_place_cursor(gpointer data) {
        auto* self = static_cast<WallpaperPicker*>(data);
        GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroll_));
        if (gtk_adjustment_get_page_size(v) == 0)
            return gtk_widget_get_mapped(self->grid_) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        self->place_source_ = 0;

        self->focus_first();
        if (self->current_index_ > 0) {
            self->move(GTK_MOVEMENT_VISUAL_POSITIONS, self->current_index_);

            // focus only scrolls it far enough to be on screen center it instead.
            GtkFlowBoxChild* child = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(self->grid_), self->current_index_);
            graphene_rect_t bounds;
            if (child && gtk_widget_compute_bounds(GTK_WIDGET(child), self->grid_, &bounds))
                gtk_adjustment_set_value(
                    v, bounds.origin.y + bounds.size.height / 2 - gtk_adjustment_get_page_size(v) / 2);
        }
        return G_SOURCE_REMOVE;
    }

    void WallpaperPicker::move(GtkMovementStep step, int count) {
        if (!grid_)
            return;
        gboolean handled = FALSE;
        g_signal_emit_by_name(grid_, "move-cursor", step, count, FALSE, FALSE, &handled);
    }

    void WallpaperPicker::focus_first() { move(GTK_MOVEMENT_BUFFER_ENDS, -1); }

    void WallpaperPicker::close() {
        pending_g_ = false;
        if (window_)
            gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }

} // namespace fenriz::desktop
