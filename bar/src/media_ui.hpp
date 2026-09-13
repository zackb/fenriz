#pragma once

#include <gtk/gtk.h>

#include <string>

#include "island.hpp"
#include "mpris.hpp"

namespace fenriz::bar {

    // The now-playing card on the island's home page, the Media page, and the track-change activity.
    class MediaUi {
    public:
        MediaUi(Island& island, Mpris& mpris);
        ~MediaUi();

        MediaUi(const MediaUi&) = delete;
        MediaUi& operator=(const MediaUi&) = delete;

    private:
        struct Controls {
            GtkWidget* previous;
            GtkWidget* play;
            GtkWidget* next;
        };

        Controls controls(const char* css_class);
        void update();
        void update_controls(const Controls& c, const Player& p);
        void load_art(const std::string& url);
        void set_art(GdkPaintable* art);
        void poll_position();

        static void on_art_loaded(GObject* source, GAsyncResult* res, gpointer data);
        static gboolean on_seek(GtkRange* range, GtkScrollType scroll, double value, gpointer data);
        static gboolean on_poll(gpointer data);

        Island& island_;
        Mpris& mpris_;

        GtkWidget* card_ = nullptr;
        GtkWidget* card_art_ = nullptr;
        GtkWidget* card_title_ = nullptr;
        GtkWidget* card_artist_ = nullptr;
        Controls card_controls_{};

        GtkWidget* page_ = nullptr;
        GtkWidget* switcher_ = nullptr;
        GtkWidget* art_ = nullptr;
        GtkWidget* title_ = nullptr;
        GtkWidget* artist_ = nullptr;
        GtkWidget* seek_ = nullptr;
        GtkWidget* elapsed_ = nullptr;
        GtkWidget* length_ = nullptr;
        Controls page_controls_{};

        std::string art_url_;
        GdkTexture* texture_ = nullptr;
        GCancellable* art_cancel_ = nullptr;
        std::string last_track_;   // bus name + title + artist, to notice a track change
        std::string switcher_for_; // player list the switcher was built from
        gint64 seeked_at_ = 0;
        guint poll_id_ = 0;
    };

} // namespace fenriz::bar
