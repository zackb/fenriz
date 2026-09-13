#include "media_ui.hpp"

#include <algorithm>

namespace fenriz::bar {

    namespace {

        constexpr gint64 SEEK_HOLD_US = 1500000; // a player reports the old position briefly after a seek

        GtkWidget* label(const char* css_class, int max_chars) {
            GtkWidget* l = gtk_label_new(nullptr);
            gtk_widget_add_css_class(l, css_class);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(l), max_chars);
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            return l;
        }

        std::string clock_text(gint64 us) {
            const gint64 s = std::max<gint64>(us, 0) / 1000000;
            char buf[32];
            g_snprintf(buf, sizeof buf, "%" G_GINT64_FORMAT ":%02d", s / 60, static_cast<int>(s % 60));
            return buf;
        }

        // A GtkImage, not a GtkPicture: a picture's natural size is the cover's own pixel size, whatever size is
        // requested.
        GtkWidget* cover(int size, const char* css_class) {
            GtkWidget* p = gtk_image_new();
            gtk_image_set_pixel_size(GTK_IMAGE(p), size);
            gtk_widget_set_halign(p, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(p, GTK_ALIGN_CENTER);
            gtk_widget_set_overflow(p, GTK_OVERFLOW_HIDDEN); // clips the image to the CSS radius
            gtk_widget_add_css_class(p, css_class);
            return p;
        }

    } // namespace

    MediaUi::MediaUi(Island& island, Mpris& mpris) : island_(island), mpris_(mpris) {
        // home card
        card_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(card_, "island-media-card");
        card_art_ = cover(44, "island-cover-small");
        GtkWidget* text_button = gtk_button_new();
        gtk_widget_add_css_class(text_button, "island-flat");
        gtk_widget_set_hexpand(text_button, TRUE);
        GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
        card_title_ = label("island-track-title", 22);
        card_artist_ = label("island-track-artist", 22);
        gtk_box_append(GTK_BOX(text), card_title_);
        gtk_box_append(GTK_BOX(text), card_artist_);
        gtk_button_set_child(GTK_BUTTON(text_button), text);
        g_signal_connect_swapped(
            text_button, "clicked", G_CALLBACK(+[](Island* island) { island->navigate("media"); }), &island_);
        card_controls_ = controls("island-icon-button");
        gtk_box_append(GTK_BOX(card_), card_art_);
        gtk_box_append(GTK_BOX(card_), text_button);
        for (GtkWidget* w : {card_controls_.previous, card_controls_.play, card_controls_.next})
            gtk_box_append(GTK_BOX(card_), w);
        gtk_widget_set_visible(card_, FALSE);
        island_.add_to_home(card_);

        // the Media page
        page_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget* header = island_.page_header("Media");
        switcher_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(switcher_, "linked");
        gtk_widget_set_hexpand(switcher_, TRUE);
        gtk_widget_set_halign(switcher_, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(header), switcher_);
        gtk_box_append(GTK_BOX(page_), header);
        art_ = cover(240, "island-cover");
        gtk_box_append(GTK_BOX(page_), art_);
        title_ = label("island-track-title", 30);
        artist_ = label("island-track-artist", 30);
        for (GtkWidget* l : {title_, artist_}) {
            gtk_label_set_xalign(GTK_LABEL(l), 0.5);
            gtk_box_append(GTK_BOX(page_), l);
        }
        GtkWidget* seek_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        elapsed_ = gtk_label_new("0:00");
        length_ = gtk_label_new("0:00");
        seek_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
        gtk_scale_set_draw_value(GTK_SCALE(seek_), FALSE);
        gtk_widget_set_hexpand(seek_, TRUE);
        gtk_widget_add_css_class(seek_, "island-slider");
        g_signal_connect(seek_, "change-value", G_CALLBACK(on_seek), this);
        for (GtkWidget* w : {elapsed_, seek_, length_}) {
            gtk_widget_add_css_class(w, "island-time-small");
            gtk_box_append(GTK_BOX(seek_row), w);
        }
        gtk_box_append(GTK_BOX(page_), seek_row);
        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
        page_controls_ = controls("island-media-button");
        for (GtkWidget* w : {page_controls_.previous, page_controls_.play, page_controls_.next})
            gtk_box_append(GTK_BOX(buttons), w);
        gtk_box_append(GTK_BOX(page_), buttons);
        island_.add_page("media", page_);

        // Position is polled, and only while the page is on screen.
        g_signal_connect_swapped(page_,
                                 "map",
                                 G_CALLBACK(+[](MediaUi* self) {
                                     if (!self->poll_id_)
                                         self->poll_id_ = g_timeout_add(1000, on_poll, self);
                                     self->poll_position();
                                 }),
                                 this);
        g_signal_connect_swapped(page_,
                                 "unmap",
                                 G_CALLBACK(+[](MediaUi* self) {
                                     if (self->poll_id_)
                                         g_source_remove(self->poll_id_);
                                     self->poll_id_ = 0;
                                 }),
                                 this);

        mpris_.subscribe([this] { update(); });
    }

    MediaUi::~MediaUi() {
        if (poll_id_)
            g_source_remove(poll_id_);
        if (art_cancel_) {
            g_cancellable_cancel(art_cancel_);
            g_object_unref(art_cancel_);
        }
        g_clear_object(&texture_);
    }

    MediaUi::Controls MediaUi::controls(const char* css_class) {
        Controls c{gtk_button_new_from_icon_name("fenriz-skip-previous-symbolic"),
                   gtk_button_new_from_icon_name("fenriz-play-symbolic"),
                   gtk_button_new_from_icon_name("fenriz-skip-next-symbolic")};
        for (GtkWidget* w : {c.previous, c.play, c.next}) {
            gtk_widget_add_css_class(w, css_class);
            gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        }
        g_signal_connect_swapped(c.previous, "clicked", G_CALLBACK(+[](Mpris* m) { m->previous(); }), &mpris_);
        g_signal_connect_swapped(c.play, "clicked", G_CALLBACK(+[](Mpris* m) { m->play_pause(); }), &mpris_);
        g_signal_connect_swapped(c.next, "clicked", G_CALLBACK(+[](Mpris* m) { m->next(); }), &mpris_);
        return c;
    }

    void MediaUi::update_controls(const Controls& c, const Player& p) {
        gtk_button_set_icon_name(GTK_BUTTON(c.play),
                                 p.playing() ? "fenriz-pause-symbolic" : "fenriz-play-symbolic");
        gtk_widget_set_sensitive(c.previous, p.can_previous);
        gtk_widget_set_sensitive(c.next, p.can_next);
    }

    void MediaUi::update() {
        const Player* p = mpris_.active();
        gtk_widget_set_visible(card_, p != nullptr);
        if (!p) {
            if (island_.expanded() && gtk_widget_get_mapped(page_))
                island_.navigate("home");
            last_track_.clear();
            return;
        }

        const Track& t = p->track;
        const std::string title = t.title.empty() ? p->label : t.title;
        for (GtkWidget* l : {card_title_, title_})
            gtk_label_set_text(GTK_LABEL(l), title.c_str());
        for (GtkWidget* l : {card_artist_, artist_})
            gtk_label_set_text(GTK_LABEL(l), t.artist.c_str());
        update_controls(card_controls_, *p);
        update_controls(page_controls_, *p);
        gtk_range_set_range(GTK_RANGE(seek_), 0, std::max<double>(t.length_us / 1e6, 1));
        gtk_widget_set_sensitive(seek_, p->can_seek && t.length_us > 0);
        const std::string length = clock_text(t.length_us);
        gtk_label_set_text(GTK_LABEL(length_), length.c_str());

        const bool new_art = t.art_url != art_url_;
        load_art(t.art_url);

        // A track change while playing is worth a glance; the first sighting of a player is not.
        const std::string key = p->bus_name + "\n" + t.title + "\n" + t.artist;
        if (key != last_track_ && p->playing() && !last_track_.empty() && !t.title.empty())
            island_.show_media(t.artist.empty() ? t.title : t.title + "  ·  " + t.artist,
                               new_art ? nullptr : GDK_PAINTABLE(texture_));
        if (p->playing() || last_track_.empty())
            last_track_ = key;

        // one toggle per player, only when there is a choice
        const auto players = mpris_.players();
        std::string names;
        for (const Player* each : players)
            names += each->bus_name + (each == p ? "*" : "") + "\n";
        if (names != switcher_for_) {
            switcher_for_ = names;
            while (GtkWidget* child = gtk_widget_get_first_child(switcher_))
                gtk_box_remove(GTK_BOX(switcher_), child);
            if (players.size() > 1)
                for (const Player* each : players) {
                    GtkWidget* b = gtk_toggle_button_new_with_label(each->label.c_str());
                    gtk_widget_add_css_class(b, "island-player");
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), each == p);
                    g_object_set_data_full(G_OBJECT(b), "bus-name", g_strdup(each->bus_name.c_str()), g_free);
                    g_signal_connect(b,
                                     "clicked",
                                     G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                         static_cast<Mpris*>(data)->pick(
                                             static_cast<const char*>(g_object_get_data(G_OBJECT(button), "bus-name")));
                                     }),
                                     &mpris_);
                    gtk_box_append(GTK_BOX(switcher_), b);
                }
        }
    }

    void MediaUi::set_art(GdkPaintable* art) {
        for (GtkWidget* pic : {card_art_, art_}) {
            gtk_image_set_from_paintable(GTK_IMAGE(pic), art);
            gtk_widget_set_visible(pic, art != nullptr);
        }
        island_.set_media_art(art);
    }

    // ponytail: http(s) covers (Spotify) load through GVfs; without it they are simply absent
    void MediaUi::load_art(const std::string& url) {
        if (url == art_url_)
            return;
        art_url_ = url;
        if (art_cancel_) {
            g_cancellable_cancel(art_cancel_);
            g_clear_object(&art_cancel_);
        }
        g_clear_object(&texture_);
        set_art(nullptr);
        if (url.empty())
            return;
        art_cancel_ = g_cancellable_new();
        GFile* file = g_file_new_for_uri(url.c_str());
        g_file_load_bytes_async(file, art_cancel_, on_art_loaded, this);
        g_object_unref(file);
    }

    void MediaUi::on_art_loaded(GObject* source, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GBytes* bytes = g_file_load_bytes_finish(G_FILE(source), res, nullptr, &err);
        if (!bytes) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_message("media: cover %s: %s", static_cast<MediaUi*>(data)->art_url_.c_str(), err->message);
            g_error_free(err);
            return;
        }
        auto* self = static_cast<MediaUi*>(data);
        GdkTexture* texture = gdk_texture_new_from_bytes(bytes, &err);
        g_bytes_unref(bytes);
        if (!texture) {
            g_message("media: cover %s: %s", self->art_url_.c_str(), err->message);
            g_error_free(err);
            return;
        }
        self->texture_ = texture;
        self->set_art(GDK_PAINTABLE(texture));
    }

    gboolean MediaUi::on_seek(GtkRange*, GtkScrollType, double value, gpointer data) {
        auto* self = static_cast<MediaUi*>(data);
        self->seeked_at_ = g_get_monotonic_time();
        self->mpris_.set_position(static_cast<gint64>(value * 1e6));
        const std::string elapsed = clock_text(static_cast<gint64>(value * 1e6));
        gtk_label_set_text(GTK_LABEL(self->elapsed_), elapsed.c_str());
        return FALSE;
    }

    gboolean MediaUi::on_poll(gpointer data) {
        static_cast<MediaUi*>(data)->poll_position();
        return G_SOURCE_CONTINUE;
    }

    void MediaUi::poll_position() {
        mpris_.query_position([this](gint64 us) {
            if (g_get_monotonic_time() - seeked_at_ < SEEK_HOLD_US)
                return;
            gtk_range_set_value(GTK_RANGE(seek_), us / 1e6);
            const std::string elapsed = clock_text(us);
            gtk_label_set_text(GTK_LABEL(elapsed_), elapsed.c_str());
        });
    }

} // namespace fenriz::bar
