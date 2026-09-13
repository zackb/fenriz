#pragma once

#include <gio/gio.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fenriz::bar {

    struct Track {
        std::string title;
        std::string artist; // xesam:artist joined with ", "
        std::string album;
        std::string art_url;
        std::string track_id; // mpris:trackid, needed by SetPosition
        gint64 length_us = 0;

        bool operator==(const Track&) const = default;
    };

    // An a{sv} xesam/mpris metadata dict. Missing keys stay empty.
    Track parse_metadata(GVariant* metadata);

    // "org.mpris.MediaPlayer2.firefox.instance_1_42" -> "Firefox"
    std::string player_label(const std::string& bus_name);

    struct Player {
        std::string bus_name;
        std::string label;
        std::string status; // Playing, Paused, Stopped
        Track track;
        bool can_next = false;
        bool can_previous = false;
        bool can_seek = false;
        gint64 active_at = 0; // monotonic time it last started playing or changed track
        GDBusProxy* proxy = nullptr;

        bool playing() const { return status == "Playing"; }
    };

    // Which player the bar follows: the one the user picked while it exists, else the most recently active one that
    // is playing, else the most recently active. -1 when there are none.
    int pick_active(const std::vector<const Player*>& players, const std::string& picked);

    // Every MPRIS player on the session bus.
    class Mpris {
    public:
        Mpris() = default;
        ~Mpris();

        Mpris(const Mpris&) = delete;
        Mpris& operator=(const Mpris&) = delete;

        void start(GDBusConnection* bus);
        void subscribe(std::function<void()> listener);

        // Null when there is no player.
        const Player* active() const;
        std::vector<const Player*> players() const;

        void pick(const std::string& bus_name);
        void play_pause();
        void next();
        void previous();
        void set_position(gint64 us);
        // Position is not signalled by players, so it is asked for.
        void query_position(std::function<void(gint64 us)> done);

    private:
        void add(const std::string& bus_name);
        void remove(const std::string& bus_name);
        void read(Player& player);
        void call(const char* method, GVariant* args);
        void notify();

        static void on_names(GObject* source, GAsyncResult* res, gpointer data);
        static void on_name_owner_changed(GDBusConnection* bus,
                                          const char* sender,
                                          const char* path,
                                          const char* iface,
                                          const char* signal,
                                          GVariant* params,
                                          gpointer data);
        static void on_proxy(GObject* source, GAsyncResult* res, gpointer data);
        static void on_properties_changed(GDBusProxy* proxy, GVariant* changed, GStrv invalidated, gpointer data);

        GDBusConnection* bus_ = nullptr;
        GCancellable* cancel_ = nullptr;
        guint subscription_ = 0;
        std::vector<std::unique_ptr<Player>> players_;
        std::string picked_;
        std::vector<std::function<void()>> listeners_;
    };

} // namespace fenriz::bar
