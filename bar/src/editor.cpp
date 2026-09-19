#include "editor.hpp"

#include <gtk4-layer-shell.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fenriz::bar {

    namespace {

        enum class Tool { Brush, Line, Arrow, Box, Ellipse, Text };

        struct ToolInfo {
            Tool tool;
            const char* glyph;
            const char* tooltip;
            guint key;
        };

        constexpr ToolInfo TOOLS[] = {
            {Tool::Brush, "✎", "Brush (B)", GDK_KEY_b},
            {Tool::Line, "╱", "Line (L)", GDK_KEY_l},
            {Tool::Arrow, "→", "Arrow (A)", GDK_KEY_a},
            {Tool::Box, "▭", "Box (R)", GDK_KEY_r},
            {Tool::Ellipse, "◯", "Ellipse (E)", GDK_KEY_e},
            {Tool::Text, "T", "Text (T)", GDK_KEY_t},
        };

        constexpr GdkRGBA COLORS[] = {
            {1.00f, 0.23f, 0.19f, 1}, // red
            {1.00f, 0.58f, 0.00f, 1}, // orange
            {1.00f, 0.80f, 0.00f, 1}, // yellow
            {0.20f, 0.78f, 0.35f, 1}, // green
            {0.04f, 0.52f, 1.00f, 1}, // blue
            {1.00f, 1.00f, 1.00f, 1},
            {0.00f, 0.00f, 0.00f, 1},
        };

        // Stroke widths in logical pixels; text is sized from the same choice.
        constexpr double WIDTHS[] = {2, 4, 8};
        constexpr double TEXT_PER_WIDTH = 7;
        constexpr double TOOLBAR_SPACE = 72; // logical pixels kept clear under the image for the toolbar
        constexpr double MARGIN = 24;

        struct Point {
            double x, y;
        };

        // In image pixels, so the final render is independent of the on-screen zoom.
        struct Shape {
            Tool tool;
            GdkRGBA color;
            double width; // stroke width, or font size for text
            std::vector<Point> points;
            std::string text;
        };

        PangoLayout* text_layout(cairo_t* cr, const Shape& s) {
            PangoLayout* layout = pango_cairo_create_layout(cr);
            PangoFontDescription* font = pango_font_description_from_string("Sans Bold");
            pango_font_description_set_absolute_size(font, s.width * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            pango_layout_set_text(layout, s.text.c_str(), -1);
            return layout;
        }

        void draw_shape(cairo_t* cr, const Shape& s) {
            if (s.points.empty())
                return;
            gdk_cairo_set_source_rgba(cr, &s.color);
            cairo_set_line_width(cr, s.width);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            const Point a = s.points.front(), b = s.points.back();
            switch (s.tool) {
            case Tool::Brush:
                cairo_move_to(cr, a.x, a.y);
                for (const Point& p : s.points)
                    cairo_line_to(cr, p.x, p.y);
                if (s.points.size() == 1)
                    cairo_line_to(cr, a.x, a.y); // a round-capped zero-length line is a dot
                cairo_stroke(cr);
                break;
            case Tool::Line:
                cairo_move_to(cr, a.x, a.y);
                cairo_line_to(cr, b.x, b.y);
                cairo_stroke(cr);
                break;
            case Tool::Arrow: {
                const double len = std::hypot(b.x - a.x, b.y - a.y);
                if (len < 1)
                    break;
                const double head = std::max(s.width * 5, 14.0);
                const double ang = std::atan2(b.y - a.y, b.x - a.x);
                const double spread = 0.45;
                // the shaft stops inside the head so its round cap does not poke through the tip
                const double shaft = std::max(0.0, len - head * 0.8);
                cairo_move_to(cr, a.x, a.y);
                cairo_line_to(cr, a.x + std::cos(ang) * shaft, a.y + std::sin(ang) * shaft);
                cairo_stroke(cr);
                cairo_move_to(cr, b.x, b.y);
                cairo_line_to(cr, b.x - head * std::cos(ang - spread), b.y - head * std::sin(ang - spread));
                cairo_line_to(cr, b.x - head * std::cos(ang + spread), b.y - head * std::sin(ang + spread));
                cairo_close_path(cr);
                cairo_fill(cr);
                break;
            }
            case Tool::Box:
                cairo_rectangle(cr, std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y));
                cairo_stroke(cr);
                break;
            case Tool::Ellipse: {
                const double rx = std::abs(b.x - a.x) / 2, ry = std::abs(b.y - a.y) / 2;
                if (rx < 0.5 || ry < 0.5)
                    break;
                cairo_save(cr);
                cairo_translate(cr, (a.x + b.x) / 2, (a.y + b.y) / 2);
                cairo_scale(cr, rx, ry);
                cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
                cairo_restore(cr); // stroke in unscaled space so the width stays uniform
                cairo_stroke(cr);
                break;
            }
            case Tool::Text: {
                PangoLayout* layout = text_layout(cr, s);
                cairo_move_to(cr, a.x, a.y);
                pango_cairo_show_layout(cr, layout);
                g_object_unref(layout);
                break;
            }
            }
        }

        struct Editor {
            GtkWindow* window = nullptr;
            GtkWidget* area = nullptr;
            cairo_surface_t* image = nullptr;
            EditorDone done;
            double ratio = 1; // image pixels per logical pixel on the monitor

            Tool tool = Tool::Arrow;
            GdkRGBA color = COLORS[0];
            double width = WIDTHS[1];
            std::vector<Shape> shapes;
            bool drawing = false;
            bool typing = false;

            // Image-to-widget mapping, refreshed on every draw.
            double scale = 1, ox = 0, oy = 0;

            std::vector<GtkWidget*> tool_buttons;

            ~Editor() { cairo_surface_destroy(image); }

            Point to_image(double x, double y) const { return {(x - ox) / scale, (y - oy) / scale}; }

            void finish_typing() {
                if (typing && shapes.back().text.empty())
                    shapes.pop_back();
                typing = false;
            }

            // Hands the result on and closes. `commit` false cancels.
            void close(bool commit) {
                finish_typing();
                cairo_surface_t* out = nullptr;
                if (commit) {
                    out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                     cairo_image_surface_get_width(image),
                                                     cairo_image_surface_get_height(image));
                    cairo_t* cr = cairo_create(out);
                    cairo_set_source_surface(cr, image, 0, 0);
                    cairo_paint(cr);
                    for (const Shape& s : shapes)
                        draw_shape(cr, s);
                    cairo_destroy(cr);
                }
                EditorDone cb = std::move(done);
                gtk_window_destroy(window);
                if (cb)
                    cb(out);
                else if (out)
                    cairo_surface_destroy(out);
            }

            void select_tool(Tool t) {
                finish_typing();
                tool = t;
                for (size_t i = 0; i < std::size(TOOLS); i++)
                    if (TOOLS[i].tool == t)
                        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tool_buttons[i]), TRUE);
                gtk_widget_queue_draw(area);
            }

            void undo() {
                typing = false;
                drawing = false;
                if (!shapes.empty())
                    shapes.pop_back();
                gtk_widget_queue_draw(area);
            }
        };

        void draw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
            auto* ed = static_cast<Editor*>(data);
            const int iw = cairo_image_surface_get_width(ed->image), ih = cairo_image_surface_get_height(ed->image);
            // fit, but never past 1:1 device pixels
            ed->scale = std::min({(w - 2 * MARGIN) / iw, (h - 2 * MARGIN - TOOLBAR_SPACE) / ih, 1 / ed->ratio});
            ed->scale = std::max(ed->scale, 0.01);
            ed->ox = std::round((w - iw * ed->scale) / 2);
            ed->oy = std::round(std::max(MARGIN, (h - TOOLBAR_SPACE - ih * ed->scale) / 2));

            cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
            cairo_paint(cr);

            cairo_save(cr);
            cairo_translate(cr, ed->ox, ed->oy);
            cairo_scale(cr, ed->scale, ed->scale);
            cairo_rectangle(cr, 0, 0, iw, ih);
            cairo_clip(cr);
            cairo_set_source_surface(cr, ed->image, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            for (const Shape& s : ed->shapes)
                draw_shape(cr, s);
            if (ed->typing) {
                // caret after the text
                const Shape& s = ed->shapes.back();
                PangoLayout* layout = text_layout(cr, s);
                int tw = 0, th = 0;
                pango_layout_get_pixel_size(layout, &tw, &th);
                g_object_unref(layout);
                gdk_cairo_set_source_rgba(cr, &s.color);
                cairo_rectangle(cr, s.points[0].x + tw + 1, s.points[0].y, std::max(1.0, s.width / 12), th);
                cairo_fill(cr);
            }
            cairo_restore(cr);
        }

        void on_drag_begin(GtkGestureDrag*, double x, double y, gpointer data) {
            auto* ed = static_cast<Editor*>(data);
            ed->finish_typing();
            const Point p = ed->to_image(x, y);
            if (ed->tool == Tool::Text) {
                ed->shapes.push_back({Tool::Text, ed->color, ed->width * TEXT_PER_WIDTH * ed->ratio, {p}, ""});
                ed->typing = true;
            } else {
                Shape s{ed->tool, ed->color, ed->width * ed->ratio, {p}, ""};
                if (ed->tool != Tool::Brush)
                    s.points.push_back(p);
                ed->shapes.push_back(std::move(s));
                ed->drawing = true;
            }
            gtk_widget_queue_draw(ed->area);
        }

        void on_drag_update(GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
            auto* ed = static_cast<Editor*>(data);
            if (!ed->drawing)
                return;
            double sx = 0, sy = 0;
            gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
            const Point p = ed->to_image(sx + dx, sy + dy);
            Shape& s = ed->shapes.back();
            if (s.tool == Tool::Brush)
                s.points.push_back(p);
            else
                s.points.back() = p;
            gtk_widget_queue_draw(ed->area);
        }

        void on_drag_end(GtkGestureDrag*, double, double, gpointer data) {
            auto* ed = static_cast<Editor*>(data);
            if (!ed->drawing)
                return;
            ed->drawing = false;
            const Shape& s = ed->shapes.back();
            // a click without a drag leaves nothing to see for anything but the brush
            if (s.tool != Tool::Brush && s.points[0].x == s.points[1].x && s.points[0].y == s.points[1].y)
                ed->shapes.pop_back();
            gtk_widget_queue_draw(ed->area);
        }

        gboolean on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType mods, gpointer data) {
            auto* ed = static_cast<Editor*>(data);
            const bool ctrl = mods & GDK_CONTROL_MASK;
            if (ed->typing && !ctrl) {
                std::string& text = ed->shapes.back().text;
                if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
                    ed->finish_typing();
                else if (keyval == GDK_KEY_BackSpace) {
                    if (!text.empty())
                        text.erase(g_utf8_find_prev_char(text.c_str(), text.c_str() + text.size()) - text.c_str());
                } else if (const gunichar c = gdk_keyval_to_unicode(keyval); c && g_unichar_isprint(c)) {
                    char buf[6];
                    text.append(buf, g_unichar_to_utf8(c, buf));
                } else
                    return FALSE;
                gtk_widget_queue_draw(ed->area);
                return TRUE;
            }
            if (ctrl && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
                ed->undo();
                return TRUE;
            }
            if (ctrl)
                return FALSE;
            if (keyval == GDK_KEY_Escape) {
                ed->close(false);
                return TRUE;
            }
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                ed->close(true);
                return TRUE;
            }
            for (const ToolInfo& t : TOOLS)
                if (keyval == t.key) {
                    ed->select_tool(t.tool);
                    return TRUE;
                }
            return FALSE;
        }

        // A dot: a colour swatch when `color` is set, a stroke width in the foreground colour otherwise.
        GtkWidget* dot(const GdkRGBA* color, double radius) {
            struct Dot {
                const GdkRGBA* color;
                double radius;
            };
            GtkWidget* area = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 18);
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 18);
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(area),
                [](GtkDrawingArea* a, cairo_t* cr, int w, int h, gpointer data) {
                    const auto* d = static_cast<Dot*>(data);
                    GdkRGBA fg;
                    gtk_widget_get_color(GTK_WIDGET(a), &fg);
                    gdk_cairo_set_source_rgba(cr, d->color ? d->color : &fg);
                    cairo_arc(cr, w / 2.0, h / 2.0, d->radius, 0, 2 * G_PI);
                    cairo_fill_preserve(cr);
                    if (d->color) { // outline so white and black read on any theme
                        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.4);
                        cairo_set_line_width(cr, 1);
                        cairo_stroke(cr);
                    }
                    cairo_new_path(cr);
                },
                new Dot{color, radius},
                [](gpointer d) { delete static_cast<Dot*>(d); });
            return area;
        }

        GtkWidget* toggle(GtkWidget* child, GtkWidget* group, const char* tooltip) {
            GtkWidget* b = gtk_toggle_button_new();
            gtk_button_set_child(GTK_BUTTON(b), child);
            if (group)
                gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group));
            if (tooltip)
                gtk_widget_set_tooltip_text(b, tooltip);
            gtk_widget_set_focusable(b, FALSE); // keys belong to the canvas
            return b;
        }

        GtkWidget* icon_button(const char* icon, const char* tooltip, GCallback cb, Editor* ed) {
            GtkWidget* b = gtk_button_new_from_icon_name(icon);
            gtk_widget_set_tooltip_text(b, tooltip);
            gtk_widget_set_focusable(b, FALSE);
            g_signal_connect_swapped(b, "clicked", cb, ed);
            return b;
        }

        GtkWidget* toolbar(Editor* ed) {
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
            gtk_widget_add_css_class(box, "shot-toolbar");
            gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(box, GTK_ALIGN_END);
            gtk_widget_set_margin_bottom(box, 16);

            GtkWidget* group = nullptr;
            for (const ToolInfo& t : TOOLS) {
                GtkWidget* b = toggle(gtk_label_new(t.glyph), group, t.tooltip);
                group = group ? group : b;
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), t.tool == ed->tool);
                g_object_set_data(G_OBJECT(b), "tool", GINT_TO_POINTER(static_cast<int>(t.tool)));
                g_signal_connect(b,
                                 "toggled",
                                 G_CALLBACK(+[](GtkToggleButton* b, Editor* ed) {
                                     if (gtk_toggle_button_get_active(b)) {
                                         ed->finish_typing();
                                         ed->tool =
                                             static_cast<Tool>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "tool")));
                                     }
                                 }),
                                 ed);
                ed->tool_buttons.push_back(b);
                gtk_box_append(GTK_BOX(box), b);
            }
            gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

            group = nullptr;
            for (const GdkRGBA& c : COLORS) {
                GtkWidget* b = toggle(dot(&c, 7), group, nullptr);
                group = group ? group : b;
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), &c == &COLORS[0]);
                g_object_set_data(G_OBJECT(b), "color", const_cast<GdkRGBA*>(&c));
                g_signal_connect(b,
                                 "toggled",
                                 G_CALLBACK(+[](GtkToggleButton* b, Editor* ed) {
                                     if (!gtk_toggle_button_get_active(b))
                                         return;
                                     ed->color = *static_cast<GdkRGBA*>(g_object_get_data(G_OBJECT(b), "color"));
                                     if (ed->typing) { // recolour the text being typed
                                         ed->shapes.back().color = ed->color;
                                         gtk_widget_queue_draw(ed->area);
                                     }
                                 }),
                                 ed);
                gtk_box_append(GTK_BOX(box), b);
            }
            gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

            group = nullptr;
            for (const double& w : WIDTHS) {
                GtkWidget* b = toggle(dot(nullptr, w / 2 + 1.5), group, nullptr);
                group = group ? group : b;
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), w == ed->width);
                g_object_set_data(G_OBJECT(b), "width", const_cast<double*>(&w));
                g_signal_connect(b,
                                 "toggled",
                                 G_CALLBACK(+[](GtkToggleButton* b, Editor* ed) {
                                     if (gtk_toggle_button_get_active(b))
                                         ed->width = *static_cast<double*>(g_object_get_data(G_OBJECT(b), "width"));
                                 }),
                                 ed);
                gtk_box_append(GTK_BOX(box), b);
            }
            gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

            gtk_box_append(
                GTK_BOX(box),
                icon_button("edit-undo-symbolic", "Undo (Ctrl+Z)", G_CALLBACK(+[](Editor* ed) { ed->undo(); }), ed));
            gtk_box_append(
                GTK_BOX(box),
                icon_button(
                    "window-close-symbolic", "Cancel (Esc)", G_CALLBACK(+[](Editor* ed) { ed->close(false); }), ed));
            gtk_box_append(
                GTK_BOX(box),
                icon_button(
                    "object-select-symbolic", "Done (Enter)", G_CALLBACK(+[](Editor* ed) { ed->close(true); }), ed));
            return box;
        }

    } // namespace

    void open_editor(GtkApplication* app, GdkMonitor* monitor, cairo_surface_t* image, EditorDone done) {
        auto* ed = new Editor;
        ed->image = image;
        ed->done = std::move(done);
        ed->ratio = std::max(1.0, gdk_monitor_get_scale(monitor));

        GtkWidget* window = gtk_application_window_new(app);
        ed->window = GTK_WINDOW(window);
        gtk_widget_add_css_class(window, "fenriz-shot");
        gtk_layer_init_for_window(ed->window);
        gtk_layer_set_namespace(ed->window, "fenriz-shot");
        gtk_layer_set_layer(ed->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_monitor(ed->window, monitor);
        for (auto edge : {GTK_LAYER_SHELL_EDGE_LEFT,
                          GTK_LAYER_SHELL_EDGE_RIGHT,
                          GTK_LAYER_SHELL_EDGE_TOP,
                          GTK_LAYER_SHELL_EDGE_BOTTOM})
            gtk_layer_set_anchor(ed->window, edge, TRUE);
        gtk_layer_set_exclusive_zone(ed->window, -1);
        gtk_layer_set_keyboard_mode(ed->window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

        ed->area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ed->area), draw, ed, nullptr);
        gtk_widget_set_cursor_from_name(ed->area, "crosshair");
        GtkGesture* drag = gtk_gesture_drag_new();
        g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), ed);
        g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), ed);
        g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), ed);
        gtk_widget_add_controller(ed->area, GTK_EVENT_CONTROLLER(drag));

        GtkWidget* overlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(overlay), ed->area);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toolbar(ed));
        gtk_window_set_child(ed->window, overlay);

        GtkEventController* keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), ed);
        gtk_widget_add_controller(window, keys);

        g_signal_connect_swapped(window, "destroy", G_CALLBACK(+[](Editor* ed) { delete ed; }), ed);
        gtk_window_present(ed->window);
    }

} // namespace fenriz::bar
