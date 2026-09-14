#include "plugin_ui.hpp"

namespace fenriz::bar {

    namespace {

        // A file path wins over an icon name; neither hides the image.
        void set_image(GtkWidget* image, const std::string& icon, const std::string& path) {
            if (!path.empty()) {
                GError* err = nullptr;
                if (GdkTexture* texture = gdk_texture_new_from_filename(path.c_str(), &err)) {
                    gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(texture));
                    g_object_unref(texture);
                    gtk_widget_set_visible(image, TRUE);
                    return;
                }
                g_warning("plugin image %s: %s", path.c_str(), err->message);
                g_error_free(err);
            }
            if (icon.empty()) {
                gtk_image_clear(GTK_IMAGE(image));
                gtk_widget_set_visible(image, FALSE);
                return;
            }
            gtk_image_set_from_icon_name(GTK_IMAGE(image), icon.c_str());
            gtk_widget_set_visible(image, TRUE);
        }

        GtkWidget* image(const std::string& icon, const std::string& path, int size) {
            GtkWidget* w = gtk_image_new();
            gtk_image_set_pixel_size(GTK_IMAGE(w), size);
            set_image(w, icon, path);
            return w;
        }

        GtkWidget* label(const std::string& text, const char* css_class) {
            GtkWidget* l = gtk_label_new(text.c_str());
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            if (css_class)
                gtk_widget_add_css_class(l, css_class);
            return l;
        }

        void clear(GtkWidget* box) {
            while (GtkWidget* child = gtk_widget_get_first_child(box))
                gtk_box_remove(GTK_BOX(box), child);
        }

    } // namespace

    PluginUi::PluginUi(Island& island, const std::string& name, const std::string& command)
        : island_(island), plugin_(name, command) {
        tile_ = gtk_button_new();
        gtk_widget_add_css_class(tile_, "island-tile");
        GtkWidget* tile_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        tile_icon_ = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(tile_icon_), 20);
        tile_label_ = label("", "island-tile-label");
        gtk_label_set_xalign(GTK_LABEL(tile_label_), 0.5);
        gtk_label_set_ellipsize(GTK_LABEL(tile_label_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(tile_label_), 12);
        gtk_box_append(GTK_BOX(tile_box), tile_icon_);
        gtk_box_append(GTK_BOX(tile_box), tile_label_);
        gtk_button_set_child(GTK_BUTTON(tile_), tile_box);
        gtk_widget_set_visible(tile_, FALSE);
        g_signal_connect_swapped(tile_, "clicked", G_CALLBACK(+[](PluginUi* self) { self->open_page(); }), this);
        island_.add_tile(tile_);

        chip_ = gtk_button_new();
        gtk_widget_add_css_class(chip_, "island-flat");
        gtk_widget_add_css_class(chip_, "island-footer-text");
        GtkWidget* chip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        chip_icon_ = gtk_image_new();
        chip_label_ = label("", nullptr);
        gtk_box_append(GTK_BOX(chip_box), chip_icon_);
        gtk_box_append(GTK_BOX(chip_box), chip_label_);
        gtk_button_set_child(GTK_BUTTON(chip_), chip_box);
        gtk_widget_set_visible(chip_, FALSE);
        g_signal_connect_swapped(chip_, "clicked", G_CALLBACK(+[](PluginUi* self) { self->open_page(); }), this);
        island_.add_to_footer(chip_);

        page_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        if (GtkWidget* builtin = island_.page(name); builtin && GTK_IS_BOX(builtin)) {
            embedded_ = true;
            gtk_box_append(GTK_BOX(builtin), page_);
        } else {
            island_.add_page(name, page_);
        }
        build_page();

        // Plugins that only need fresh data while someone is looking can wait for these.
        island_.on_open([this] { plugin_.send(plugin_event("open")); });
        island_.on_close([this] { plugin_.send(plugin_event("close")); });
        plugin_.on_change([this](unsigned slots) { changed(slots); });
        plugin_.start();
    }

    void PluginUi::open_page() {
        if (plugin_.state().page || embedded_)
            island_.navigate(plugin_.name());
    }

    void PluginUi::changed(unsigned slots) {
        const PluginState& s = plugin_.state();
        if ((slots & SLOT_PILL) && s.pill) {
            GdkTexture* texture =
                s.pill->image.empty() ? nullptr : gdk_texture_new_from_filename(s.pill->image.c_str(), nullptr);
            island_.show_notice(s.pill->icon.empty() ? nullptr : s.pill->icon.c_str(),
                                texture ? GDK_PAINTABLE(texture) : nullptr,
                                s.pill->text);
            if (texture)
                g_object_unref(texture);
        }
        if (slots & SLOT_TILE) {
            gtk_widget_set_visible(tile_, s.tile.has_value());
            if (s.tile) {
                set_image(tile_icon_, s.tile->icon, s.tile->image);
                gtk_label_set_text(GTK_LABEL(tile_label_), s.tile->title.c_str());
            }
        }
        if (slots & SLOT_CHIP) {
            gtk_widget_set_visible(chip_, s.chip.has_value());
            if (s.chip) {
                set_image(chip_icon_, s.chip->icon, {});
                gtk_label_set_text(GTK_LABEL(chip_label_), s.chip->text.c_str());
            }
        }
        if (slots & SLOT_PAGE)
            build_page();
    }

    // Rebuilt whole on every change; plugins report every few minutes at most.
    void PluginUi::build_page() {
        clear(page_);
        const auto& page = plugin_.state().page;
        if (embedded_) {
            if (!page)
                return;
            if (!page->title.empty())
                gtk_box_append(GTK_BOX(page_), label(page->title, "island-section"));
        } else {
            gtk_box_append(
                GTK_BOX(page_),
                island_.page_header(page && !page->title.empty() ? page->title.c_str() : plugin_.name().c_str()));
        }
        if (!page) {
            GtkWidget* empty = label("Nothing yet", "island-empty");
            gtk_label_set_xalign(GTK_LABEL(empty), 0.5);
            gtk_box_append(GTK_BOX(page_), empty);
            return;
        }

        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        // under a built-in page the list shares the screen with that page's own content
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), embedded_ ? 260 : 420);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
        GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        for (const PluginBlock& b : page->blocks)
            if (GtkWidget* w = block(b))
                gtk_box_append(GTK_BOX(body), w);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), body);
        gtk_box_append(GTK_BOX(page_), scroller);
    }

    GtkWidget* PluginUi::block(const PluginBlock& b) {
        if (b.type == "row")
            return row(b);

        if (b.type == "text") {
            const char* css = b.style == "dim"       ? "island-dim"
                              : b.style == "section" ? "island-section"
                              : b.style == "title"   ? "island-title"
                                                     : nullptr;
            GtkWidget* l = label(b.text, css);
            gtk_label_set_wrap(GTK_LABEL(l), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
            return l;
        }

        if (b.type == "list") {
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            for (const PluginBlock& item : b.items)
                gtk_box_append(GTK_BOX(box), row(item));
            return box;
        }

        if (b.type == "table") {
            GtkWidget* grid = gtk_grid_new();
            gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
            gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
            gtk_widget_add_css_class(grid, "island-table");
            int y = 0;
            if (!b.columns.empty()) {
                for (size_t x = 0; x < b.columns.size(); x++) {
                    GtkWidget* l = label(b.columns[x], "island-section");
                    gtk_label_set_xalign(GTK_LABEL(l), x ? 1 : 0);
                    gtk_grid_attach(GTK_GRID(grid), l, static_cast<int>(x), y, 1, 1);
                }
                y++;
            }
            for (size_t r = 0; r < b.rows.size(); r++, y++)
                for (size_t x = 0; x < b.rows[r].size(); x++) {
                    GtkWidget* l = label(b.rows[r][x], "island-cell");
                    gtk_label_set_xalign(GTK_LABEL(l), x ? 1 : 0);
                    gtk_widget_set_hexpand(l, x == 0);
                    if (static_cast<int>(r) == b.highlight)
                        gtk_widget_add_css_class(l, "island-highlight");
                    gtk_grid_attach(GTK_GRID(grid), l, static_cast<int>(x), y, 1, 1);
                }
            return grid;
        }

        if (b.type == "level") {
            GtkWidget* level = gtk_level_bar_new_for_interval(0, 1);
            gtk_level_bar_set_value(GTK_LEVEL_BAR(level), b.value);
            gtk_widget_add_css_class(level, "island-level");
            return level;
        }

        if (b.type == "button") {
            GtkWidget* button = gtk_button_new_with_label(b.text.c_str());
            gtk_widget_add_css_class(button, "island-action");
            return actionable(button, b.action);
        }

        g_warning("plugin %s: unknown block type '%s'", plugin_.name().c_str(), b.type.c_str());
        return nullptr;
    }

    GtkWidget* PluginUi::row(const PluginBlock& b) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        if (!b.icon.empty() || !b.image.empty()) {
            GtkWidget* img = image(b.icon, b.image, 24);
            gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(box), img);
        }
        GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
        GtkWidget* title = label(b.text, "island-track-title");
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(title), 30);
        gtk_box_append(GTK_BOX(text), title);
        if (!b.subtitle.empty()) {
            GtkWidget* sub = label(b.subtitle, "island-track-artist");
            gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(sub), 34);
            gtk_box_append(GTK_BOX(text), sub);
        }
        gtk_box_append(GTK_BOX(box), text);
        if (!b.trailing.empty())
            gtk_box_append(GTK_BOX(box), label(b.trailing, "island-stat"));

        if (b.action.empty()) {
            gtk_widget_add_css_class(box, "island-device");
            return box;
        }
        GtkWidget* button = gtk_button_new();
        gtk_widget_add_css_class(button, "island-device-button");
        gtk_widget_add_css_class(button, "island-device-row");
        gtk_button_set_child(GTK_BUTTON(button), box);
        return actionable(button, b.action);
    }

    GtkWidget* PluginUi::actionable(GtkWidget* button, const std::string& action) {
        if (action.empty())
            return button;
        g_object_set_data_full(G_OBJECT(button), "plugin-action", g_strdup(action.c_str()), g_free);
        g_signal_connect(button,
                         "clicked",
                         G_CALLBACK(+[](GtkButton* b, gpointer data) {
                             const auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "plugin-action"));
                             static_cast<PluginUi*>(data)->send(plugin_event("action", id));
                         }),
                         this);
        return button;
    }

} // namespace fenriz::bar
