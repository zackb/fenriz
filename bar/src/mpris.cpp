#include "mpris.hpp"

#include <algorithm>

namespace fenriz::bar {

    namespace {

        constexpr const char* PREFIX = "org.mpris.MediaPlayer2.";
        constexpr const char* PATH = "/org/mpris/MediaPlayer2";
        constexpr const char* PLAYER_IFACE = "org.mpris.MediaPlayer2.Player";

        std::string lookup_string(GVariant* dict, const char* key) {
            const char* s = nullptr;
            return g_variant_lookup(dict, key, "&s", &s) && s ? s : "";
        }

        bool cached_bool(GDBusProxy* proxy, const char* name) {
            GVariant* v = g_dbus_proxy_get_cached_property(proxy, name);
            const bool b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
            if (v)
                g_variant_unref(v);
            return b;
        }

        struct ProxyRequest {
            class Mpris* mpris;
            std::string bus_name;
        };

    } // namespace

    Track parse_metadata(GVariant* metadata) {
        Track t;
        if (!metadata || !g_variant_is_of_type(metadata, G_VARIANT_TYPE_VARDICT))
            return t;
        t.title = lookup_string(metadata, "xesam:title");
        t.album = lookup_string(metadata, "xesam:album");
        t.art_url = lookup_string(metadata, "mpris:artUrl");

        // players disagree on the type: spec says o, some send s
        if (GVariant* id = g_variant_lookup_value(metadata, "mpris:trackid", nullptr)) {
            if (g_variant_is_of_type(id, G_VARIANT_TYPE_OBJECT_PATH) || g_variant_is_of_type(id, G_VARIANT_TYPE_STRING))
                t.track_id = g_variant_get_string(id, nullptr);
            g_variant_unref(id);
        }
        if (GVariant* len = g_variant_lookup_value(metadata, "mpris:length", nullptr)) {
            if (g_variant_is_of_type(len, G_VARIANT_TYPE_INT64))
                t.length_us = g_variant_get_int64(len);
            else if (g_variant_is_of_type(len, G_VARIANT_TYPE_UINT64))
                t.length_us = static_cast<gint64>(g_variant_get_uint64(len));
            g_variant_unref(len);
        }
        if (GVariant* artists = g_variant_lookup_value(metadata, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY)) {
            gsize n = 0;
            const char** list = g_variant_get_strv(artists, &n);
            for (gsize i = 0; i < n; i++)
                t.artist += (i ? ", " : "") + std::string(list[i]);
            g_free(list);
            g_variant_unref(artists);
        } else {
            t.artist = lookup_string(metadata, "xesam:artist"); // a bare string, against the spec
        }
        return t;
    }

    std::string player_label(const std::string& bus_name) {
        std::string name =
            bus_name.rfind(PREFIX, 0) == 0 ? bus_name.substr(std::char_traits<char>::length(PREFIX)) : bus_name;
        name = name.substr(0, name.find('.')); // drop ".instance_…"
        if (!name.empty())
            name[0] = g_ascii_toupper(name[0]);
        return name;
    }

    int pick_active(const std::vector<const Player*>& players, const std::string& picked) {
        int best = -1;
        for (int i = 0; i < static_cast<int>(players.size()); i++) {
            const Player& p = *players[i];
            if (!picked.empty() && p.bus_name == picked)
                return i;
            if (best < 0)
                best = i;
            else if (p.playing() != players[best]->playing() ? p.playing() : p.active_at > players[best]->active_at)
                best = i;
        }
        return best;
    }

    Mpris::~Mpris() {
        if (subscription_)
            g_dbus_connection_signal_unsubscribe(bus_, subscription_);
        if (cancel_) {
            g_cancellable_cancel(cancel_);
            g_object_unref(cancel_);
        }
        for (auto& p : players_)
            g_clear_object(&p->proxy);
    }

    void Mpris::start(GDBusConnection* bus) {
        if (!bus)
            return;
        bus_ = bus;
        cancel_ = g_cancellable_new();
        subscription_ = g_dbus_connection_signal_subscribe(bus,
                                                           "org.freedesktop.DBus",
                                                           "org.freedesktop.DBus",
                                                           "NameOwnerChanged",
                                                           "/org/freedesktop/DBus",
                                                           "org.mpris.MediaPlayer2",
                                                           G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
                                                           on_name_owner_changed,
                                                           this,
                                                           nullptr);
        g_dbus_connection_call(bus,
                               "org.freedesktop.DBus",
                               "/org/freedesktop/DBus",
                               "org.freedesktop.DBus",
                               "ListNames",
                               nullptr,
                               G_VARIANT_TYPE("(as)"),
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               cancel_,
                               on_names,
                               this);
    }

    void Mpris::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void Mpris::on_names(GObject* source, GAsyncResult* res, gpointer data) {
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr);
        if (!reply)
            return; // cancelled, or no bus to speak of
        auto* self = static_cast<Mpris*>(data);
        GVariantIter* names = nullptr;
        const char* name = nullptr;
        g_variant_get(reply, "(as)", &names);
        while (g_variant_iter_next(names, "&s", &name))
            if (g_str_has_prefix(name, PREFIX))
                self->add(name);
        g_variant_iter_free(names);
        g_variant_unref(reply);
    }

    void Mpris::on_name_owner_changed(
        GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* params, gpointer data) {
        auto* self = static_cast<Mpris*>(data);
        const char* name = nullptr;
        const char* old_owner = nullptr;
        const char* new_owner = nullptr;
        g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (!g_str_has_prefix(name, PREFIX))
            return;
        if (*old_owner)
            self->remove(name);
        if (*new_owner)
            self->add(name);
    }

    void Mpris::add(const std::string& bus_name) {
        if (std::any_of(players_.begin(), players_.end(), [&](const auto& p) { return p->bus_name == bus_name; }))
            return;
        auto player = std::make_unique<Player>();
        player->bus_name = bus_name;
        player->label = player_label(bus_name);
        players_.push_back(std::move(player));
        g_dbus_proxy_new(bus_,
                         G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                         nullptr,
                         bus_name.c_str(),
                         PATH,
                         PLAYER_IFACE,
                         cancel_,
                         on_proxy,
                         new ProxyRequest{this, bus_name});
    }

    void Mpris::on_proxy(GObject*, GAsyncResult* res, gpointer data) {
        std::unique_ptr<ProxyRequest> req(static_cast<ProxyRequest*>(data));
        GError* err = nullptr;
        GDBusProxy* proxy = g_dbus_proxy_new_finish(res, &err);
        if (!proxy) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                g_message("mpris: %s: %s", req->bus_name.c_str(), err->message);
            g_error_free(err);
            return;
        }
        Mpris* self = req->mpris;
        auto it = std::find_if(self->players_.begin(), self->players_.end(), [&](const auto& p) {
            return p->bus_name == req->bus_name && !p->proxy;
        });
        if (it == self->players_.end()) { // gone while the proxy was being made
            g_object_unref(proxy);
            return;
        }
        Player& player = **it;
        player.proxy = proxy;
        g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_properties_changed), self);
        self->read(player);
        self->notify();
    }

    void Mpris::remove(const std::string& bus_name) {
        auto it =
            std::find_if(players_.begin(), players_.end(), [&](const auto& p) { return p->bus_name == bus_name; });
        if (it == players_.end())
            return;
        if ((*it)->proxy) {
            g_signal_handlers_disconnect_by_data((*it)->proxy, this);
            g_object_unref((*it)->proxy);
        }
        players_.erase(it);
        if (picked_ == bus_name)
            picked_.clear();
        notify();
    }

    void Mpris::read(Player& player) {
        const std::string was_status = player.status;
        const Track was_track = player.track;

        GVariant* status = g_dbus_proxy_get_cached_property(player.proxy, "PlaybackStatus");
        player.status = status ? g_variant_get_string(status, nullptr) : "Stopped";
        if (status)
            g_variant_unref(status);
        GVariant* metadata = g_dbus_proxy_get_cached_property(player.proxy, "Metadata");
        player.track = parse_metadata(metadata);
        if (metadata)
            g_variant_unref(metadata);
        player.can_next = cached_bool(player.proxy, "CanGoNext");
        player.can_previous = cached_bool(player.proxy, "CanGoPrevious");
        player.can_seek = cached_bool(player.proxy, "CanSeek");

        if ((player.playing() && was_status != "Playing") || (player.playing() && !(was_track == player.track)))
            player.active_at = g_get_monotonic_time();
    }

    void Mpris::on_properties_changed(GDBusProxy* proxy, GVariant*, GStrv, gpointer data) {
        auto* self = static_cast<Mpris*>(data);
        for (auto& p : self->players_)
            if (p->proxy == proxy) {
                self->read(*p);
                self->notify();
                return;
            }
    }

    void Mpris::notify() {
        for (auto& listener : listeners_)
            listener();
    }

    std::vector<const Player*> Mpris::players() const {
        std::vector<const Player*> out;
        for (const auto& p : players_)
            if (p->proxy)
                out.push_back(p.get());
        return out;
    }

    const Player* Mpris::active() const {
        const auto list = players();
        const int i = pick_active(list, picked_);
        return i < 0 ? nullptr : list[i];
    }

    void Mpris::pick(const std::string& bus_name) {
        picked_ = bus_name;
        notify();
    }

    void Mpris::call(const char* method, GVariant* args) {
        const Player* p = active();
        if (!p)
            return;
        g_dbus_proxy_call(p->proxy, method, args, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
    }

    void Mpris::play_pause() { call("PlayPause", nullptr); }
    void Mpris::next() { call("Next", nullptr); }
    void Mpris::previous() { call("Previous", nullptr); }

    void Mpris::set_position(gint64 us) {
        const Player* p = active();
        if (!p || !p->can_seek || p->track.track_id.empty())
            return;
        call("SetPosition", g_variant_new("(ox)", p->track.track_id.c_str(), us));
    }

    void Mpris::query_position(std::function<void(gint64)> done) {
        const Player* p = active();
        if (!p)
            return;
        auto* cb = new std::function<void(gint64)>(std::move(done));
        g_dbus_connection_call(
            bus_,
            p->bus_name.c_str(),
            PATH,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", PLAYER_IFACE, "Position"),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            cancel_,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                std::unique_ptr<std::function<void(gint64)>> cb(static_cast<std::function<void(gint64)>*>(data));
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr);
                if (!reply)
                    return;
                GVariant* v = nullptr;
                g_variant_get(reply, "(v)", &v);
                if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
                    (*cb)(g_variant_get_int64(v));
                g_variant_unref(v);
                g_variant_unref(reply);
            },
            cb);
    }

} // namespace fenriz::bar
