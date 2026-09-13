#include "audio_ui.hpp"

#include <cmath>

#include "volume.hpp"

namespace fenriz::bar {

    namespace {

        GtkWidget* icon_button(const char* icon, const char* css_class) {
            GtkWidget* b = gtk_button_new_from_icon_name(icon);
            gtk_widget_add_css_class(b, css_class);
            gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
            return b;
        }

        GtkWidget* slider() {
            GtkWidget* s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
            gtk_scale_set_draw_value(GTK_SCALE(s), FALSE);
            gtk_widget_set_hexpand(s, TRUE);
            gtk_widget_add_css_class(s, "island-slider");
            return s;
        }

        GtkWidget* row() {
            GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_add_css_class(r, "island-slider-row");
            return r;
        }

        GtkWidget* section_label(const char* text) {
            GtkWidget* l = gtk_label_new(text);
            gtk_widget_add_css_class(l, "island-section");
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            return l;
        }

        int rounded(double value) { return static_cast<int>(std::lround(value)); }

    } // namespace

    AudioUi::AudioUi(Island& island, Audio& audio, desktop::Brightness& brightness)
        : island_(island), audio_(audio), brightness_(brightness) {
        // home: volume
        volume_row_ = row();
        mute_ = icon_button("audio-volume-high-symbolic", "island-icon-button");
        g_signal_connect_swapped(
            mute_,
            "clicked",
            G_CALLBACK(+[](AudioUi* self) { self->audio_.set_muted(false, !self->audio_.sink().muted); }),
            this);
        volume_ = slider();
        g_signal_connect(volume_, "change-value", G_CALLBACK(on_volume), this);
        GtkWidget* more = icon_button("go-next-symbolic", "island-icon-button");
        g_signal_connect_swapped(
            more, "clicked", G_CALLBACK(+[](Island* island) { island->navigate("audio"); }), &island_);
        gtk_box_append(GTK_BOX(volume_row_), mute_);
        gtk_box_append(GTK_BOX(volume_row_), volume_);
        gtk_box_append(GTK_BOX(volume_row_), more);
        gtk_widget_set_visible(volume_row_, FALSE);
        island_.add_to_home(volume_row_);

        // home: brightness
        brightness_row_ = row();
        GtkWidget* sun = gtk_image_new_from_icon_name("display-brightness-symbolic");
        gtk_widget_add_css_class(sun, "island-row-icon");
        brightness_scale_ = slider();
        gtk_range_set_range(GTK_RANGE(brightness_scale_), 1, 100); // 0 reads as a dead screen
        g_signal_connect(brightness_scale_, "change-value", G_CALLBACK(on_brightness), this);
        gtk_box_append(GTK_BOX(brightness_row_), sun);
        gtk_box_append(GTK_BOX(brightness_row_), brightness_scale_);
        GtkWidget* align = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); // stands in for the volume row's chevron
        gtk_widget_add_css_class(align, "island-row-icon");
        gtk_box_append(GTK_BOX(brightness_row_), align);
        gtk_widget_set_visible(brightness_row_, brightness_.available());
        island_.add_to_home(brightness_row_);
        island_.on_open([this] { brightness_changed(brightness_.percent()); });

        // the Sound page
        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_append(GTK_BOX(page), island_.page_header("Sound"));
        gtk_box_append(GTK_BOX(page), section_label("Output"));
        outputs_.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(page), outputs_.list);
        gtk_box_append(GTK_BOX(page), section_label("Input"));
        inputs_.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(page), inputs_.list);
        GtkWidget* mic = row();
        mic_mute_ = icon_button("audio-input-microphone-symbolic", "island-icon-button");
        g_signal_connect_swapped(
            mic_mute_,
            "clicked",
            G_CALLBACK(+[](AudioUi* self) { self->audio_.set_muted(true, !self->audio_.source().muted); }),
            this);
        mic_volume_ = slider();
        g_signal_connect(mic_volume_, "change-value", G_CALLBACK(on_mic_volume), this);
        gtk_box_append(GTK_BOX(mic), mic_mute_);
        gtk_box_append(GTK_BOX(mic), mic_volume_);
        gtk_box_append(GTK_BOX(page), mic);
        island_.add_page("audio", page);

        audio_.subscribe([this] { update(); });
    }

    void AudioUi::brightness_changed(int percent) {
        if (percent >= 0)
            gtk_range_set_value(GTK_RANGE(brightness_scale_), percent);
    }

    void AudioUi::update() {
        const Audio::Endpoint& sink = audio_.sink();
        const Audio::Endpoint& source = audio_.source();
        updating_ = true;

        gtk_widget_set_visible(volume_row_, sink.id != 0);
        if (sink.percent >= 0)
            gtk_range_set_value(GTK_RANGE(volume_), sink.percent);
        gtk_button_set_icon_name(GTK_BUTTON(mute_), desktop::volume_icon(sink.percent, sink.muted));

        gtk_widget_set_sensitive(mic_volume_, source.id != 0);
        if (source.percent >= 0)
            gtk_range_set_value(GTK_RANGE(mic_volume_), source.percent);
        gtk_button_set_icon_name(GTK_BUTTON(mic_mute_), desktop::mic_icon(source.muted));

        rebuild(outputs_, sink);
        rebuild(inputs_, source);
        updating_ = false;
    }

    // Rows are recreated only when the device list changed, so a volume change never rebuilds them under the pointer.
    void AudioUi::rebuild(Section& section, const Audio::Endpoint& ep) {
        if (ep.devices != section.shown) {
            section.shown = ep.devices;
            while (GtkWidget* child = gtk_widget_get_first_child(section.list))
                gtk_box_remove(GTK_BOX(section.list), child);
            GtkCheckButton* group = nullptr;
            for (const Audio::Device& dev : ep.devices) {
                GtkWidget* button = gtk_check_button_new_with_label(dev.description.c_str());
                gtk_widget_add_css_class(button, "island-device");
                if (group)
                    gtk_check_button_set_group(GTK_CHECK_BUTTON(button), group);
                else
                    group = GTK_CHECK_BUTTON(button);
                g_object_set_data_full(G_OBJECT(button), "node-name", g_strdup(dev.name.c_str()), g_free);
                g_object_set_data(G_OBJECT(button), "node-id", GUINT_TO_POINTER(dev.id));
                g_object_set_data(G_OBJECT(button), "section", &section);
                g_signal_connect(button, "toggled", G_CALLBACK(on_device_toggled), this);
                gtk_box_append(GTK_BOX(section.list), button);
            }
        }
        for (GtkWidget* child = gtk_widget_get_first_child(section.list); child;
             child = gtk_widget_get_next_sibling(child))
            gtk_check_button_set_active(GTK_CHECK_BUTTON(child),
                                        GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "node-id")) == ep.id);
    }

    void AudioUi::on_device_toggled(GtkCheckButton* button, gpointer data) {
        auto* self = static_cast<AudioUi*>(data);
        if (self->updating_ || !gtk_check_button_get_active(button))
            return;
        auto* section = static_cast<Section*>(g_object_get_data(G_OBJECT(button), "section"));
        self->audio_.set_default(section->source,
                                 static_cast<const char*>(g_object_get_data(G_OBJECT(button), "node-name")));
    }

    gboolean AudioUi::on_volume(GtkRange*, GtkScrollType, double value, gpointer data) {
        static_cast<AudioUi*>(data)->audio_.set_percent(false, rounded(value));
        return FALSE;
    }

    gboolean AudioUi::on_mic_volume(GtkRange*, GtkScrollType, double value, gpointer data) {
        static_cast<AudioUi*>(data)->audio_.set_percent(true, rounded(value));
        return FALSE;
    }

    gboolean AudioUi::on_brightness(GtkRange*, GtkScrollType, double value, gpointer data) {
        static_cast<AudioUi*>(data)->brightness_.set_percent(rounded(value));
        return FALSE;
    }

} // namespace fenriz::bar
