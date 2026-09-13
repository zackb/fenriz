#include <cassert>

#include "mpris.hpp"

using fenriz::bar::parse_metadata;
using fenriz::bar::pick_active;
using fenriz::bar::Player;
using fenriz::bar::player_label;

namespace {

    GVariant* metadata(const char* text) {
        GError* err = nullptr;
        GVariant* v = g_variant_parse(G_VARIANT_TYPE_VARDICT, text, nullptr, nullptr, &err);
        assert(v && !err);
        return g_variant_ref_sink(v);
    }

    void test_spec_metadata() {
        GVariant* m = metadata(
            "{'mpris:trackid': <objectpath '/org/mpris/MediaPlayer2/Track/7'>, 'mpris:length': <int64 215000000>,"
            " 'mpris:artUrl': <'file:///tmp/cover.png'>, 'xesam:title': <'Song'>, 'xesam:album': <'Album'>,"
            " 'xesam:artist': <['A', 'B']>}");
        const auto t = parse_metadata(m);
        assert(t.title == "Song");
        assert(t.artist == "A, B");
        assert(t.album == "Album");
        assert(t.art_url == "file:///tmp/cover.png");
        assert(t.track_id == "/org/mpris/MediaPlayer2/Track/7");
        assert(t.length_us == 215000000);
        g_variant_unref(m);
    }

    // Real players break the spec: a string artist, a string trackid, an unsigned length.
    void test_off_spec_metadata() {
        GVariant* m =
            metadata("{'mpris:trackid': <'spotify:track:1'>, 'mpris:length': <uint64 5>, 'xesam:artist': <'Solo'>}");
        const auto t = parse_metadata(m);
        assert(t.artist == "Solo");
        assert(t.track_id == "spotify:track:1");
        assert(t.length_us == 5);
        assert(t.title.empty());
        g_variant_unref(m);
        assert(parse_metadata(nullptr).title.empty());
    }

    void test_label() {
        assert(player_label("org.mpris.MediaPlayer2.spotify") == "Spotify");
        assert(player_label("org.mpris.MediaPlayer2.firefox.instance_1_42") == "Firefox");
        assert(player_label("org.mpris.MediaPlayer2.") == "");
    }

    Player player(const char* name, const char* status, gint64 active_at) {
        Player p;
        p.bus_name = name;
        p.status = status;
        p.active_at = active_at;
        return p;
    }

    void test_pick() {
        Player paused_recent = player("a", "Paused", 30);
        Player playing_old = player("b", "Playing", 10);
        Player playing_new = player("c", "Playing", 20);

        assert(pick_active({}, "") == -1);
        assert(pick_active({&paused_recent}, "") == 0);
        // playing beats a more recent pause, and the latest to start playing wins
        assert(pick_active({&paused_recent, &playing_old, &playing_new}, "") == 2);
        assert(pick_active({&paused_recent, &playing_new, &playing_old}, "") == 1);
        // the user's pick sticks, even paused
        assert(pick_active({&paused_recent, &playing_old, &playing_new}, "a") == 0);
        // a pick that has gone away is ignored
        assert(pick_active({&playing_old, &playing_new}, "a") == 1);
    }

} // namespace

int main() {
    test_spec_metadata();
    test_off_spec_metadata();
    test_label();
    test_pick();
    return 0;
}
