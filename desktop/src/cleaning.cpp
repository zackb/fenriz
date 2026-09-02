#include "cleaning.hpp"

#include <gio/gunixsocketaddress.h>
#include <gtk4-layer-shell.h>

#include <cstdlib>

#include "blur.hpp"
#include "spawn.hpp"

namespace fenriz::desktop {

    namespace {

        // How long the menu entry asks for.
        constexpr int kSeconds = 60;

        std::string field(const std::string& line, const char* key) {
            const std::string pat = std::string("\"") + key + "\":\"";
            const size_t p = line.find(pat);
            if (p == std::string::npos)
                return "";
            const size_t start = p + pat.size();
            const size_t end = line.find('"', start);
            return end == std::string::npos ? "" : line.substr(start, end - start);
        }

        int number(const std::string& line, const char* key) {
            const std::string pat = std::string("\"") + key + "\":";
            const size_t p = line.find(pat);
            return p == std::string::npos ? 0 : atoi(line.c_str() + p + pat.size());
        }

    } // namespace

    Cleaning::~Cleaning() {
        hide();
        if (confirm_)
            gtk_window_destroy(confirm_);
        g_clear_object(&in_);
        g_clear_object(&conn_);
    }

    void Cleaning::start(GtkApplication* app) {
        app_ = app;
        const char* path = getenv("FENRIZ_EVENT_SOCKET");
        if (!path || !*path) {
            g_message("cleaning: no FENRIZ_EVENT_SOCKET; the countdown overlay is off");
            return;
        }

        GSocketClient* client = g_socket_client_new();
        GSocketAddress* addr = g_unix_socket_address_new(path);
        GError* err = nullptr;
        conn_ = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), nullptr, &err);
        g_object_unref(addr);
        g_object_unref(client);
        if (!conn_) {
            g_message("cleaning: %s", err->message);
            g_error_free(err);
            return;
        }
        in_ = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn_)));
        read_line();
    }

    void Cleaning::read_line() { g_data_input_stream_read_line_async(in_, G_PRIORITY_DEFAULT, nullptr, on_line, this); }

    void Cleaning::on_line(GObject* source, GAsyncResult* res, gpointer data) {
        auto* self = static_cast<Cleaning*>(data);
        gsize len = 0;
        char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), res, &len, nullptr);
        if (!line) // compositor gone; the session is going with it
            return;

        const std::string s(line, len);
        g_free(line);
        if (s.find("\"event\":\"cleaning\"") != std::string::npos) {
            const int seconds = number(s, "seconds");
            if (seconds > 0)
                self->show(seconds, field(s, "cancel"));
            else
                self->hide();
        }
        self->read_line();
    }

    void Cleaning::show(int seconds, const std::string& cancel) {
        hide();
        remaining_ = seconds;
        g_message("cleaning: input off for %ds", seconds);

        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        const guint n = g_list_model_get_n_items(monitors);
        for (guint i = 0; i < n; i++) {
            GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, i));
            g_object_unref(monitor); // the list keeps its own ref
            add_monitor(monitor, cancel);
        }
        // ponytail: monitors plugged in mid-mode get no overlay; the mode is a minute long.

        update_labels();
        tick_ = g_timeout_add_seconds(1, on_tick, this);
    }

    void Cleaning::add_monitor(GdkMonitor* monitor, const std::string& cancel) {
        GtkWidget* scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_add_css_class(scrim, "cleaning-scrim");
        gtk_widget_set_valign(scrim, GTK_ALIGN_CENTER);

        GtkWidget* title = gtk_label_new("Keyboard cleaning");
        gtk_widget_add_css_class(title, "cleaning-label");
        gtk_box_append(GTK_BOX(scrim), title);

        GtkWidget* time = gtk_label_new("");
        gtk_widget_add_css_class(time, "cleaning-time");
        gtk_box_append(GTK_BOX(scrim), time);

        const std::string hint = cancel.empty() ? "Run `fenrizctl cleaning off` to end it early"
                                                : "Input is off. Press " + cancel + " to end it early";
        GtkWidget* label = gtk_label_new(hint.c_str());
        gtk_widget_add_css_class(label, "cleaning-hint");
        gtk_box_append(GTK_BOX(scrim), label);

        GtkWindow* window = GTK_WINDOW(gtk_application_window_new(app_));
        gtk_layer_init_for_window(window);
        gtk_layer_set_namespace(window, "fenriz-cleaning");
        gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_monitor(window, monitor);
        for (int edge = 0; edge < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; edge++)
            gtk_layer_set_anchor(window, static_cast<GtkLayerShellEdge>(edge), TRUE);
        gtk_layer_set_exclusive_zone(window, -1);
        // The compositor is already dropping input; an exclusive grab would buy nothing.
        gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_widget_add_css_class(GTK_WIDGET(window), "fenriz-shell");
        gtk_window_set_child(window, scrim);

        surfaces_[monitor] = window;
        times_[monitor] = time;
        gtk_window_present(window);
    }

    void Cleaning::update_labels() {
        char buf[16];
        g_snprintf(buf, sizeof(buf), "%d:%02d", remaining_ / 60, remaining_ % 60);
        for (auto& [monitor, label] : times_)
            gtk_label_set_text(GTK_LABEL(label), buf);
    }

    gboolean Cleaning::on_tick(gpointer data) {
        auto* self = static_cast<Cleaning*>(data);
        if (self->remaining_ > 0)
            self->remaining_--;
        self->update_labels();

        return G_SOURCE_CONTINUE;
    }

    void Cleaning::hide() {
        if (tick_) {
            g_source_remove(tick_);
            tick_ = 0;
        }
        for (auto& [monitor, window] : surfaces_)
            gtk_window_destroy(window);
        surfaces_.clear();
        times_.clear();
        remaining_ = 0;
    }

    void Cleaning::on_confirm_response(GtkWidget* button, gpointer data) {
        auto* self = static_cast<Cleaning*>(data);
        const bool go = g_object_get_data(G_OBJECT(button), "start") != nullptr;
        if (self->confirm_) {
            gtk_window_destroy(self->confirm_);
            self->confirm_ = nullptr;
        }
        if (go)
            spawn::command("fenrizctl cleaning " + std::to_string(kSeconds));
    }

    gboolean Cleaning::on_confirm_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
        if (keyval != GDK_KEY_Escape)
            return FALSE;
        auto* self = static_cast<Cleaning*>(data);
        if (self->confirm_) {
            gtk_window_destroy(self->confirm_);
            self->confirm_ = nullptr;
        }
        return TRUE;
    }

    void Cleaning::confirm() {
        if (confirm_) {
            gtk_window_present(confirm_);
            return;
        }

        confirm_ = GTK_WINDOW(gtk_window_new());
        gtk_layer_init_for_window(confirm_);
        gtk_layer_set_namespace(confirm_, "fenriz-cleaning-confirm");
        gtk_layer_set_layer(confirm_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(confirm_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_widget_add_css_class(GTK_WIDGET(confirm_), "fenriz-shell");

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_add_css_class(box, "fenriz-cleaning");
        gtk_widget_set_size_request(box, 420, -1);

        GtkWidget* title = gtk_label_new("Disable the keyboard and touchpad for one minute so "
                                         "they can be wiped down?");
        gtk_label_set_wrap(GTK_LABEL(title), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(title), 44);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_box_append(GTK_BOX(box), title);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);
        GtkWidget* cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect(cancel, "clicked", G_CALLBACK(on_confirm_response), this);
        gtk_box_append(GTK_BOX(buttons), cancel);
        GtkWidget* go = gtk_button_new_with_label("Start");
        g_object_set_data(G_OBJECT(go), "start", go);
        gtk_widget_add_css_class(go, "suggested-action");
        g_signal_connect(go, "clicked", G_CALLBACK(on_confirm_response), this);
        gtk_box_append(GTK_BOX(buttons), go);
        gtk_box_append(GTK_BOX(box), buttons);

        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_confirm_key), this);
        gtk_widget_add_controller(GTK_WIDGET(confirm_), GTK_EVENT_CONTROLLER(keys));

        gtk_window_set_child(confirm_, box);
        gtk_window_set_focus(confirm_, go);
        blur::attach(GTK_NATIVE(confirm_));
        gtk_window_present(confirm_);
    }

} // namespace fenriz::desktop
