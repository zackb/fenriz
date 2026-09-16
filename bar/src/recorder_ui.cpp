#include "recorder_ui.hpp"

#include <algorithm>

namespace fenriz::bar {

    namespace {

        constexpr const char* ICON = "fenriz-record-symbolic";

        std::string elapsed_text(int seconds) {
            char buf[16];
            if (seconds >= 3600)
                g_snprintf(buf, sizeof buf, "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
            else
                g_snprintf(buf, sizeof buf, "%d:%02d", seconds / 60, seconds % 60);
            return buf;
        }

        GtkWidget* section_label(const char* text) {
            GtkWidget* l = gtk_label_new(text);
            gtk_widget_add_css_class(l, "island-section");
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            return l;
        }

    } // namespace

    RecorderUi::RecorderUi(Island& island, Recorder& recorder, Audio& audio, Compositor& compositor)
        : island_(island), recorder_(recorder), audio_(audio), compositor_(compositor) {

        chip_ = gtk_button_new();
        gtk_widget_add_css_class(chip_, "island-flat");
        gtk_widget_set_tooltip_text(chip_, "Record this screen · right-click for audio");
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(ICON));
        chip_label_ = gtk_label_new(nullptr);
        gtk_widget_add_css_class(chip_label_, "island-footer-text");
        gtk_widget_set_visible(chip_label_, FALSE);
        gtk_box_append(GTK_BOX(box), chip_label_);
        gtk_button_set_child(GTK_BUTTON(chip_), box);
        g_signal_connect_swapped(chip_, "clicked", G_CALLBACK(+[](RecorderUi* self) { self->toggle(); }), this);

        // right-click opens the page instead of recording
        GtkGesture* secondary = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
        g_signal_connect_swapped(secondary, "pressed", G_CALLBACK(+[](Island* i) { i->navigate("record"); }), &island_);
        gtk_widget_add_controller(chip_, GTK_EVENT_CONTROLLER(secondary));
        island_.add_to_footer(chip_);

        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(page), island_.page_header("Record"));
        gtk_box_append(GTK_BOX(page), section_label("Audio"));
        sources_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(page), sources_);
        island_.add_page("record", page);

        audio_.subscribe([this] { rebuild_sources(); });
        rebuild_sources();

        recorder_.on_changed([this] { update(); });
        recorder_.on_finished([this](const std::string& path, bool ok) { finished(path, ok); });
        update();
    }

    RecorderUi::~RecorderUi() {
        if (tick_id_)
            g_source_remove(tick_id_);
    }

    void RecorderUi::toggle() {
        if (recorder_.recording()) {
            recorder_.stop();
            return;
        }
        if (recorder_.start(compositor_.state().focused_output(), device_))
            island_.show_event(ICON, "Recording");
        else
            island_.show_event("dialog-warning-symbolic", "Could not start recording");
        update();
    }

    GtkWidget* RecorderUi::create_status() {
        GtkWidget* capsule = gtk_button_new();
        gtk_widget_add_css_class(capsule, "bar-capsule");
        gtk_widget_add_css_class(capsule, "bar-recording");
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(ICON));
        gtk_box_append(GTK_BOX(box), gtk_label_new(nullptr));
        gtk_button_set_child(GTK_BUTTON(capsule), box);
        g_signal_connect_swapped(capsule, "clicked", G_CALLBACK(+[](RecorderUi* self) { self->toggle(); }), this);
        g_signal_connect(
            capsule,
            "destroy",
            G_CALLBACK(+[](GtkWidget* w, gpointer data) { std::erase(static_cast<RecorderUi*>(data)->statuses_, w); }),
            this);
        statuses_.push_back(capsule);
        gtk_widget_set_visible(capsule, recorder_.recording());
        return capsule;
    }

    void RecorderUi::update() {
        const bool recording = recorder_.recording();
        const std::string text = recording ? elapsed_text(recorder_.elapsed_seconds()) : "";

        gtk_label_set_text(GTK_LABEL(chip_label_), text.c_str());
        gtk_widget_set_visible(chip_label_, recording);
        if (recording)
            gtk_widget_add_css_class(chip_, "recording");
        else
            gtk_widget_remove_css_class(chip_, "recording");

        for (GtkWidget* capsule : statuses_) {
            gtk_widget_set_visible(capsule, recording);
            if (!recording)
                continue;
            GtkWidget* box = gtk_button_get_child(GTK_BUTTON(capsule));
            GtkWidget* label = gtk_widget_get_last_child(box);
            gtk_label_set_text(GTK_LABEL(label), text.c_str());
        }

        if (recording && !tick_id_)
            tick_id_ = g_timeout_add_seconds(1, on_tick, this);
        else if (!recording && tick_id_) {
            g_source_remove(tick_id_);
            tick_id_ = 0;
        }
    }

    gboolean RecorderUi::on_tick(gpointer data) {
        static_cast<RecorderUi*>(data)->update();
        return G_SOURCE_CONTINUE;
    }

    // Rows are recreated only when the devices changed.
    void RecorderUi::rebuild_sources() {
        std::vector<std::pair<std::string, std::string>> choices = {{"No audio", ""}};
        for (const Audio::Device& dev : audio_.source().devices)
            choices.emplace_back(dev.description, dev.name);
        for (const Audio::Device& dev : audio_.sink().devices)
            choices.emplace_back(dev.description + " (system)", dev.name + ".monitor");
        if (choices == shown_)
            return;
        shown_ = choices;

        // A device that went away falls back to no audio rather than recording silence from a dead node.
        if (std::none_of(choices.begin(), choices.end(), [this](const auto& c) { return c.second == device_; }))
            device_.clear();

        while (GtkWidget* child = gtk_widget_get_first_child(sources_))
            gtk_box_remove(GTK_BOX(sources_), child);
        GtkCheckButton* group = nullptr;
        for (const auto& [label, device] : choices) {
            GtkWidget* button = gtk_check_button_new_with_label(label.c_str());
            gtk_widget_add_css_class(button, "island-device");
            if (group)
                gtk_check_button_set_group(GTK_CHECK_BUTTON(button), group);
            else
                group = GTK_CHECK_BUTTON(button);
            g_object_set_data_full(G_OBJECT(button), "device", g_strdup(device.c_str()), g_free);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(button), device == device_);
            g_signal_connect(button,
                             "toggled",
                             G_CALLBACK(+[](GtkCheckButton* b, gpointer data) {
                                 if (!gtk_check_button_get_active(b))
                                     return;
                                 static_cast<RecorderUi*>(data)->device_ =
                                     static_cast<const char*>(g_object_get_data(G_OBJECT(b), "device"));
                             }),
                             this);
            gtk_box_append(GTK_BOX(sources_), button);
        }
    }

    void RecorderUi::finished(const std::string& path, bool ok) {
        if (!ok) {
            island_.show_event("dialog-warning-symbolic", "Recording failed");
            return;
        }
        island_.show_event(ICON, "Recording saved");
        notify(path);
    }

    // fenriz-desktop is the session's notification daemon; nothing happens when no daemon is running.
    void RecorderUi::notify(const std::string& path) {
        GError* err = nullptr;
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
        if (!bus) {
            g_warning("recorder: no session bus: %s", err->message);
            g_error_free(err);
            return;
        }
        GVariantBuilder actions, hints;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        g_dbus_connection_call(
            bus,
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "Notify",
            g_variant_new(
                "(susssasa{sv}i)", "fenriz-bar", 0u, ICON, "Recording saved", path.c_str(), &actions, &hints, -1),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            [](GObject* source, GAsyncResult* res, gpointer) {
                GError* err = nullptr;
                if (GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err))
                    g_variant_unref(r);
                if (err) {
                    g_debug("recorder: notify: %s", err->message);
                    g_error_free(err);
                }
            },
            nullptr);
        g_object_unref(bus);
    }

} // namespace fenriz::bar
