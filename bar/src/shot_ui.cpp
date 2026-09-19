#include "shot_ui.hpp"

#include <gio/gunixsocketaddress.h>
#include <gtk4-layer-shell.h>

#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "capture.hpp"
#include "clipboard.hpp"
#include "editor.hpp"

namespace fenriz::bar {

    namespace {

        constexpr const char* SHOT_ICON = "fenriz-camera-symbolic";
        constexpr const char* CANCELLED = "cancelled";

        // Connect-time state
        std::optional<ShotLayout> fetch_layout(std::string& error) {
            const char* path = g_getenv("FENRIZ_SOCKET");
            if (!path || !*path) {
                error = "FENRIZ_SOCKET is not set";
                return std::nullopt;
            }
            GError* err = nullptr;
            GSocketClient* client = g_socket_client_new();
            GSocketAddress* addr = g_unix_socket_address_new(path);
            GSocketConnection* conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), nullptr, &err);
            g_object_unref(addr);
            g_object_unref(client);
            if (!conn) {
                error = err->message;
                g_error_free(err);
                return std::nullopt;
            }
            GDataInputStream* in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
            gsize len = 0;
            char* line = g_data_input_stream_read_line(in, &len, nullptr, &err);
            std::optional<ShotLayout> layout;
            if (line)
                layout = parse_layout(std::string(line, len));
            if (!layout)
                error = err ? err->message : "no state from the compositor";
            g_clear_error(&err);
            g_free(line);
            g_object_unref(in);
            g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
            g_object_unref(conn);
            return layout;
        }

        GdkMonitor* monitor_named(const std::string& connector) {
            GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
            for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
                auto* m = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i));
                g_object_unref(m); // the list keeps it alive
                const char* c = gdk_monitor_get_connector(m);
                if (c && connector == c)
                    return m;
            }
            return nullptr;
        }

        GtkWidget* section_label(const char* text) {
            GtkWidget* l = gtk_label_new(text);
            gtk_widget_add_css_class(l, "island-section");
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            return l;
        }

        cairo_surface_t* crop(cairo_surface_t* src, const Rect& px) {
            cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px.width, px.height);
            cairo_t* cr = cairo_create(out);
            cairo_set_source_surface(cr, src, -px.x, -px.y);
            cairo_paint(cr);
            cairo_destroy(cr);
            return out;
        }

        GBytes* encode_png(cairo_surface_t* image) {
            GByteArray* buf = g_byte_array_new();
            cairo_surface_write_to_png_stream(
                image,
                [](void* closure, const unsigned char* data, unsigned int length) {
                    g_byte_array_append(static_cast<GByteArray*>(closure), data, length);
                    return CAIRO_STATUS_SUCCESS;
                },
                buf);
            return g_byte_array_free_to_bytes(buf);
        }

        std::string default_path() {
            const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
            char* dir = g_build_filename(pictures ? pictures : g_get_home_dir(), "Screenshots", nullptr);
            char* path = g_build_filename(dir, shot_filename(std::time(nullptr)).c_str(), nullptr);
            std::string out = path;
            g_free(path);
            g_free(dir);
            return out;
        }

    } // namespace

    // One screenshot from command to delivery
    struct Shot {
        struct Pane {
            Shot* shot = nullptr;
            ShotLayout::Output output;
            GdkMonitor* monitor = nullptr;
            cairo_surface_t* image = nullptr;
            GtkWindow* window = nullptr;
            GtkWidget* area = nullptr;
            bool dragging = false;
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            Rect hover;

            // Window under a pane point, clipped to this output, in pane coordinates; empty when none.
            Rect window_under(double x, double y) const {
                const ShotLayout::Window* w = shot->layout.window_at(output,
                                                                     output.box.x + static_cast<int>(std::lround(x)),
                                                                     output.box.y + static_cast<int>(std::lround(y)));
                if (!w)
                    return {};
                const Rect r = w->box.intersect(output.box);
                return {r.x - output.box.x, r.y - output.box.y, r.width, r.height};
            }

            ~Pane() {
                if (window) {
                    detach_handlers(GTK_WIDGET(window), this);
                    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), nullptr, nullptr, nullptr);
                    gtk_window_destroy(window);
                }
                if (image)
                    cairo_surface_destroy(image);
            }
        };

        ShotUi& ui;
        ShotArgs args;
        GApplicationCommandLine* cmdline;
        ShotLayout layout;
        std::vector<std::unique_ptr<Pane>> panes;
        int pending = 0; // captures in flight

        // `cmdline` is null for a shot started from the island.
        Shot(ShotUi& ui, ShotArgs args, GApplicationCommandLine* cmdline)
            : ui(ui)
            , args(std::move(args))
            , cmdline(cmdline ? G_APPLICATION_COMMAND_LINE(g_object_ref(cmdline)) : nullptr) {}

        ~Shot() {
            panes.clear();
            if (cmdline)
                g_object_unref(cmdline); // releases the waiting client
        }

        // Ends the shot. Deletes this. Errors go to the command that asked, or to the island.
        void end(int status, const std::string& error = "") {
            if (cmdline) {
                if (!error.empty())
                    g_application_command_line_printerr(cmdline, "%s\n", error.c_str());
                g_application_command_line_set_exit_status(cmdline, status);
            } else if (!error.empty() && error != CANCELLED)
                ui.island_.show_event("dialog-warning-symbolic", "Screenshot failed: " + error);
            if (status == 0)
                ui.island_.show_event(SHOT_ICON, args.copy ? "Screenshot copied" : "Screenshot saved");
            ui.status_ = status;
            ui.shot_.reset();
        }

        void start() {
            std::string error;
            auto l = fetch_layout(error);
            if (!l)
                return end(1, "compositor: " + error);
            layout = std::move(*l);
            if (!capture_available())
                return end(1, "the compositor does not offer ext-image-copy-capture-v1");

            const ShotLayout::Output* target = layout.focused_output();
            Rect crop_to;
            if (args.mode == ShotMode::Window) {
                const ShotLayout::Window* w = layout.focused_window();
                if (!w)
                    return end(1, "no focused window");
                target = layout.output_of(w->box);
                crop_to = w->box;
            }
            if (!target)
                return end(1, "no output");

            // region freezes every screen
            for (const ShotLayout::Output& o : layout.outputs) {
                if (args.mode != ShotMode::Region && o.name != target->name)
                    continue;
                GdkMonitor* m = monitor_named(o.name);
                if (!m)
                    continue;
                auto pane = std::make_unique<Pane>();
                pane->shot = this;
                pane->output = o;
                pane->monitor = m;
                panes.push_back(std::move(pane));
            }
            if (panes.empty())
                return end(1, "no output to capture");

            pending = static_cast<int>(panes.size());
            for (auto& p : panes)
                capture_output(p->monitor, [this, pane = p.get(), crop_to](cairo_surface_t* image) {
                    pane->image = image;
                    if (--pending == 0)
                        captured(crop_to);
                });
        }

        void captured(const Rect& crop_to) {
            std::erase_if(panes, [](const auto& p) { return p->image == nullptr; });
            if (panes.empty())
                return end(1, "capture failed");
            if (args.mode == ShotMode::Region)
                return select();
            Pane& p = *panes.front();
            cairo_surface_t* image = p.image;
            p.image = nullptr;
            if (!crop_to.empty()) {
                const Rect px = to_pixels(
                    crop_to, p.output.box, cairo_image_surface_get_width(image), cairo_image_surface_get_height(image));
                cairo_surface_t* whole = image;
                image = px.empty() ? nullptr : crop(whole, px);
                cairo_surface_destroy(whole);
                if (!image)
                    return end(1, "the window is off screen");
            }
            chosen(image, p.monitor);
        }

        // Takes `image` annotates it if asked, then delivers.
        void chosen(cairo_surface_t* image, GdkMonitor* monitor) {
            if (!args.edit)
                return deliver(image);
            open_editor(ui.app_, monitor, image, [this](cairo_surface_t* result) {
                if (!result)
                    return end(1, CANCELLED);
                deliver(result);
            });
        }

        void deliver(cairo_surface_t* image) {
            std::string failure;
            if (args.copy) {
                GBytes* png = encode_png(image);
                if (!copy_png(png))
                    failure = "the compositor does not offer ext-data-control-v1";
                g_bytes_unref(png);
            }
            if (args.save) {
                std::string path = args.path.empty() ? default_path() : args.path;
                GFile* file = cmdline ? g_application_command_line_create_file_for_arg(cmdline, path.c_str())
                                      : g_file_new_for_commandline_arg(path.c_str());
                char* abs = g_file_get_path(file);
                g_object_unref(file);
                path = abs ? abs : path;
                g_free(abs);
                char* dir = g_path_get_dirname(path.c_str());
                g_mkdir_with_parents(dir, 0700);
                g_free(dir);
                const cairo_status_t st = cairo_surface_write_to_png(image, path.c_str());
                if (st != CAIRO_STATUS_SUCCESS)
                    failure = path + ": " + cairo_status_to_string(st);
                else if (cmdline)
                    g_application_command_line_print(cmdline, "%s\n", path.c_str());
            }
            cairo_surface_destroy(image);
            end(failure.empty() ? 0 : 1, failure);
        }

        void select() {
            for (auto& p : panes)
                open_pane(*p);
        }

        void open_pane(Pane& p) {
            GtkWidget* window = gtk_application_window_new(ui.app_);
            p.window = GTK_WINDOW(window);
            gtk_widget_add_css_class(window, "fenriz-shot");
            gtk_layer_init_for_window(p.window);
            gtk_layer_set_namespace(p.window, "fenriz-shot");
            gtk_layer_set_layer(p.window, GTK_LAYER_SHELL_LAYER_OVERLAY);
            gtk_layer_set_monitor(p.window, p.monitor);
            for (auto edge : {GTK_LAYER_SHELL_EDGE_LEFT,
                              GTK_LAYER_SHELL_EDGE_RIGHT,
                              GTK_LAYER_SHELL_EDGE_TOP,
                              GTK_LAYER_SHELL_EDGE_BOTTOM})
                gtk_layer_set_anchor(p.window, edge, TRUE);
            gtk_layer_set_exclusive_zone(p.window, -1);
            gtk_layer_set_keyboard_mode(p.window,
                                        p.output.focused ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
                                                         : GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

            p.area = gtk_drawing_area_new();
            gtk_widget_add_css_class(p.area, "shot-selection");
            gtk_widget_set_cursor_from_name(p.area, "crosshair");
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p.area), draw_pane, &p, nullptr);

            GtkGesture* drag = gtk_gesture_drag_new();
            g_signal_connect(drag,
                             "drag-begin",
                             G_CALLBACK(+[](GtkGestureDrag*, double x, double y, Pane* p) {
                                 p->dragging = true;
                                 p->x0 = p->x1 = x;
                                 p->y0 = p->y1 = y;
                             }),
                             &p);
            g_signal_connect(drag,
                             "drag-update",
                             G_CALLBACK(+[](GtkGestureDrag*, double dx, double dy, Pane* p) {
                                 p->x1 = p->x0 + dx;
                                 p->y1 = p->y0 + dy;
                                 gtk_widget_queue_draw(p->area);
                             }),
                             &p);
            g_signal_connect(drag,
                             "drag-end",
                             G_CALLBACK(+[](GtkGestureDrag*, double, double, Pane* p) {
                                 p->dragging = false;
                                 p->shot->picked(*p);
                             }),
                             &p);
            gtk_widget_add_controller(p.area, GTK_EVENT_CONTROLLER(drag));

            GtkEventController* motion = gtk_event_controller_motion_new();
            g_signal_connect(motion,
                             "motion",
                             G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, Pane* p) {
                                 const Rect before = p->hover;
                                 p->hover = p->window_under(x, y);
                                 if (!(before == p->hover))
                                     gtk_widget_queue_draw(p->area);
                             }),
                             &p);
            gtk_widget_add_controller(p.area, motion);

            GtkEventController* keys = gtk_event_controller_key_new();
            g_signal_connect(
                keys,
                "key-pressed",
                G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, Pane* p) -> gboolean {
                    if (keyval != GDK_KEY_Escape)
                        return FALSE;
                    p->shot->end(1, CANCELLED);
                    return TRUE;
                }),
                &p);
            gtk_widget_add_controller(window, keys);

            gtk_window_set_child(p.window, p.area);
            gtk_window_present(p.window);
        }

        static void draw_pane(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer data) {
            const auto* p = static_cast<Pane*>(data);
            const int iw = cairo_image_surface_get_width(p->image), ih = cairo_image_surface_get_height(p->image);
            cairo_save(cr);
            cairo_scale(cr, static_cast<double>(w) / iw, static_cast<double>(h) / ih);
            cairo_set_source_surface(cr, p->image, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);

            const Rect sel = p->dragging ? Rect::spanning(static_cast<int>(std::lround(p->x0)),
                                                          static_cast<int>(std::lround(p->y0)),
                                                          static_cast<int>(std::lround(p->x1)),
                                                          static_cast<int>(std::lround(p->y1)))
                                         : p->hover;
            // dim everything but the selection
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_rectangle(cr, 0, 0, w, h);
            if (!sel.empty())
                cairo_rectangle(cr, sel.x, sel.y, sel.width, sel.height);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
            cairo_fill(cr);
            if (sel.empty())
                return;
            GdkRGBA accent;
            gtk_widget_get_color(GTK_WIDGET(area), &accent);
            gdk_cairo_set_source_rgba(cr, &accent);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, sel.x - 1, sel.y - 1, sel.width + 2, sel.height + 2);
            cairo_stroke(cr);
        }

        // Drag or click finished on `p`
        void picked(Pane& p) {
            const ShotLayout::Output& o = p.output;
            Rect sel = Rect::spanning(static_cast<int>(std::lround(p.x0)),
                                      static_cast<int>(std::lround(p.y0)),
                                      static_cast<int>(std::lround(p.x1)),
                                      static_cast<int>(std::lround(p.y1)));
            if (sel.width < 4 && sel.height < 4) {
                sel = p.window_under(p.x0, p.y0);
                if (sel.empty())
                    sel = {0, 0, o.box.width, o.box.height};
            }
            const Rect logical = {sel.x + o.box.x, sel.y + o.box.y, sel.width, sel.height};
            const Rect px = to_pixels(
                logical, o.box, cairo_image_surface_get_width(p.image), cairo_image_surface_get_height(p.image));
            if (px.empty())
                return end(1, "empty selection");
            cairo_surface_t* image = crop(p.image, px);
            GdkMonitor* monitor = p.monitor;
            panes.clear(); // closes every selector
            chosen(image, monitor);
        }
    };

    ShotUi::ShotUi(GtkApplication* app, Island& island) : app_(app), island_(island) {
        GtkWidget* chip = gtk_button_new_from_icon_name(SHOT_ICON);
        gtk_widget_add_css_class(chip, "island-flat");
        gtk_widget_set_tooltip_text(chip, "Screenshot · right-click for options");
        g_signal_connect_swapped(chip, "clicked", G_CALLBACK(+[](ShotUi* self) { self->take_from_island(); }), this);
        GtkGesture* secondary = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
        g_signal_connect_swapped(secondary, "pressed", G_CALLBACK(+[](Island* i) { i->navigate("shot"); }), &island_);
        gtk_widget_add_controller(chip, GTK_EVENT_CONTROLLER(secondary));
        island_.add_to_footer(chip);

        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(page), island_.page_header("Screenshot"));
        gtk_box_append(GTK_BOX(page), section_label("Capture"));
        GtkCheckButton* group = nullptr;
        for (const auto& [label, mode] : {std::pair{"Region", ShotMode::Region},
                                          std::pair{"Window", ShotMode::Window},
                                          std::pair{"Screen", ShotMode::Screen}}) {
            GtkWidget* b = gtk_check_button_new_with_label(label);
            gtk_widget_add_css_class(b, "island-device");
            if (group)
                gtk_check_button_set_group(GTK_CHECK_BUTTON(b), group);
            else
                group = GTK_CHECK_BUTTON(b);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(b), mode == options_.mode);
            g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(static_cast<int>(mode)));
            g_signal_connect(b,
                             "toggled",
                             G_CALLBACK(+[](GtkCheckButton* b, ShotUi* self) {
                                 if (gtk_check_button_get_active(b))
                                     self->options_.mode =
                                         static_cast<ShotMode>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode")));
                             }),
                             this);
            gtk_box_append(GTK_BOX(page), b);
        }
        gtk_box_append(GTK_BOX(page), section_label("Then"));
        for (const auto& [label, flag] : {std::pair{"Annotate", &options_.edit},
                                          std::pair{"Copy to clipboard", &options_.copy},
                                          std::pair{"Save to Pictures", &options_.save}}) {
            GtkWidget* b = gtk_check_button_new_with_label(label);
            gtk_widget_add_css_class(b, "island-device");
            gtk_check_button_set_active(GTK_CHECK_BUTTON(b), *flag);
            g_signal_connect(b,
                             "toggled",
                             G_CALLBACK(+[](GtkCheckButton* b, bool* flag) { *flag = gtk_check_button_get_active(b); }),
                             flag);
            gtk_box_append(GTK_BOX(page), b);
        }
        island_.add_page("shot", page);
    }

    ShotUi::~ShotUi() {
        if (delay_id_)
            g_source_remove(delay_id_);
    }

    void ShotUi::take_from_island() {
        if (!options_.copy && !options_.save) {
            island_.show_event("dialog-warning-symbolic", "Pick copy or save");
            return;
        }
        if (shot_ || delay_id_)
            return;
        island_.close();
        delay_id_ = g_timeout_add_once(
            450,
            [](gpointer data) {
                auto* self = static_cast<ShotUi*>(data);
                self->delay_id_ = 0;
                self->take(self->options_, nullptr);
            },
            this);
    }

    int ShotUi::take(const ShotArgs& args, GApplicationCommandLine* cmdline) {
        if (shot_) {
            if (cmdline)
                g_application_command_line_printerr(cmdline, "a screenshot is already in progress\n");
            return 1;
        }
        shot_ = std::make_unique<Shot>(*this, args, cmdline);
        shot_->start();
        return shot_ ? 0 : status_;
    }

} // namespace fenriz::bar
