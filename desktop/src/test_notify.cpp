#include "notify.hpp"
#include "toast.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

using namespace fenriz::desktop;

namespace {

    void test_expiry() {
        // An explicit timeout always wins, whatever the urgency.
        assert(notify_expiry_ms(3000, 1, 5000) == 3000);
        assert(notify_expiry_ms(3000, 2, 5000) == 3000);

        // 0 means "never expire" and is not the same as "unset".
        assert(notify_expiry_ms(0, 0, 5000) == 0);
        assert(notify_expiry_ms(0, 2, 5000) == 0);

        // Negative defers to the server: the config timeout, unless it is critical.
        assert(notify_expiry_ms(-1, 0, 5000) == 5000);
        assert(notify_expiry_ms(-1, 1, 5000) == 5000);
        assert(notify_expiry_ms(-1, 2, 5000) == 0);
    }

    void test_body_markup() {
        assert(notify_body_markup("").empty());
        assert(notify_body_markup("plain text") == "plain text");
        assert(notify_body_markup("<b>bold</b>") == "<b>bold</b>");

        // Malformed markup must degrade to literal text, never to an empty label.
        const std::string unclosed = notify_body_markup("<b>oops");
        assert(unclosed == "&lt;b&gt;oops");

        // <img> is not pango markup, and chat clients send it anyway.
        const std::string img = notify_body_markup("see <img src=\"x\"> this");
        assert(img.find("&lt;img") != std::string::npos);

        // A bare ampersand is invalid markup and is the most common real-world case.
        assert(notify_body_markup("Tom & Jerry") == "Tom &amp; Jerry");
    }

    void test_actions() {
        const Actions none = notify_split_actions({});
        assert(none.buttons.empty());
        assert(!none.has_default);

        const Actions pair = notify_split_actions({"open", "Open", "later", "Later"});
        assert(pair.buttons.size() == 2);
        assert(pair.buttons[0].first == "open");
        assert(pair.buttons[0].second == "Open");
        assert(pair.buttons[1].first == "later");
        assert(!pair.has_default);

        // "default" fires on a click anywhere, so it never becomes a button.
        const Actions with_default = notify_split_actions({"default", "Open", "dismiss", "Dismiss"});
        assert(with_default.has_default);
        assert(with_default.buttons.size() == 1);
        assert(with_default.buttons[0].first == "dismiss");

        // A trailing key with no label is dropped rather than read past the end.
        const Actions odd = notify_split_actions({"open", "Open", "orphan"});
        assert(odd.buttons.size() == 1);
    }

    void test_anchors() {
        const Anchors top_right = notify_anchors("top-right");
        assert(top_right.top && top_right.right && !top_right.left);

        const Anchors bottom_left = notify_anchors("bottom-left");
        assert(!bottom_left.top && bottom_left.left && !bottom_left.right);

        // Centred means anchored to neither side.
        const Anchors top_center = notify_anchors("top-center");
        assert(top_center.top && !top_center.left && !top_center.right);

        // Anything unparseable lands on the default rather than off-screen.
        for (const char* bad : {"", "sideways", "top", "middle-right", "top-middle", "-"}) {
            const Anchors fallback = notify_anchors(bad);
            assert(fallback.top && fallback.right && !fallback.left);
        }
    }

    void test_hint_bool() {
        // The spec's own type.
        GVariant* hints = g_variant_new_parsed("{'transient': <true>}");
        g_variant_ref_sink(hints);
        assert(notify_hint_bool(hints, "transient"));
        assert(!notify_hint_bool(hints, "resident")); // absent is false, not an error
        g_variant_unref(hints);

        hints = g_variant_ref_sink(g_variant_new_parsed("{'transient': <false>}"));
        assert(!notify_hint_bool(hints, "transient"));
        g_variant_unref(hints);

        // What senders actually put on the wire.
        for (const char* truthy : {"<byte 1>", "<int32 1>", "<uint32 1>", "<'1'>", "<'true'>"}) {
            char* text = g_strdup_printf("{'transient': %s}", truthy);
            hints = g_variant_ref_sink(g_variant_new_parsed(text));
            assert(notify_hint_bool(hints, "transient"));
            g_variant_unref(hints);
            g_free(text);
        }
        for (const char* falsy : {"<byte 0>", "<int32 0>", "<uint32 0>", "<'0'>", "<'false'>"}) {
            char* text = g_strdup_printf("{'transient': %s}", falsy);
            hints = g_variant_ref_sink(g_variant_new_parsed(text));
            assert(!notify_hint_bool(hints, "transient"));
            g_variant_unref(hints);
            g_free(text);
        }
    }

    Toast make_toast(guint32 id, const char* summary) {
        Toast toast;
        toast.id = id;
        toast.summary = summary;
        return toast;
    }

    void test_history() {
        Config cfg = Config::parse("notify_history = 3");
        Notifications notifications(nullptr, cfg);

        notifications.remember(make_toast(1, "one"), "app");
        notifications.remember(make_toast(2, "two"), "app");
        assert(notifications.history().size() == 2);
        // Newest first.
        assert(notifications.history().front().summary == "two");
        assert(notifications.history().front().app_name == "app");
        assert(notifications.history().front().time > 0);

        // The cap drops the oldest, not the newest.
        notifications.remember(make_toast(3, "three"), "app");
        notifications.remember(make_toast(4, "four"), "app");
        assert(notifications.history().size() == 3);
        assert(notifications.history().front().summary == "four");
        assert(notifications.history().back().summary == "two");

        // A replacing notification updates its entry rather than adding a second one.
        notifications.remember(make_toast(3, "three, again"), "app");
        assert(notifications.history().size() == 3);
        assert(notifications.history().front().summary == "three, again");
        assert(std::count_if(notifications.history().begin(), notifications.history().end(), [](const HistoryItem& i) {
                   return i.id == 3;
               }) == 1);

        notifications.dismiss_history(999); // unknown id is a no-op, not a crash
        assert(notifications.history().size() == 3);
        notifications.dismiss_history(3);
        assert(notifications.history().size() == 2);

        notifications.clear_history();
        assert(notifications.history().empty());
        notifications.clear_history(); // idempotent
        assert(notifications.history().empty());
    }

    void test_history_disabled() {
        Config cfg = Config::parse("notify_history = 0");
        Notifications notifications(nullptr, cfg);
        notifications.remember(make_toast(1, "one"), "app");
        assert(notifications.history().empty());
    }

    // A sender may put a huge image in a hint; the cap holds dozens of entries.
    void test_history_texture_cap() {
        auto texture = [](int size) {
            GBytes* bytes = g_bytes_new_take(g_malloc0(size * size * 4), size * size * 4);
            GdkTexture* out = gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8A8, bytes, size * 4);
            g_bytes_unref(bytes);
            return out;
        };

        Config cfg = Config::parse("notify_history = 5");
        Notifications notifications(nullptr, cfg);

        Toast small = make_toast(1, "small");
        small.texture = texture(64);
        notifications.remember(small, "app");
        assert(notifications.history().front().texture != nullptr);
        g_object_unref(small.texture);

        Toast big = make_toast(2, "big");
        big.texture = texture(512);
        big.icon = "dialog-information";
        notifications.remember(big, "app");
        assert(notifications.history().front().texture == nullptr);
        assert(notifications.history().front().icon == "dialog-information");
        g_object_unref(big.texture);
    }

    void test_config() {
        const Config cfg = Config::parse("notifications = off\n"
                                         "notify_timeout = 12000\n"
                                         "notify_position = bottom-left\n"
                                         "notify_history = 200\n");
        assert(!cfg.notifications);
        assert(cfg.notify_timeout == 12000);
        assert(cfg.notify_position == "bottom-left");
        assert(cfg.notify_history == 200);

        assert(Config::parse("notify_history = 0").notify_history == 0);
        assert(Config::parse("notify_history = 9999").notify_history == 500);
        assert(Config::parse("notify_history = lots").notify_history == 50);

        // Out of range clamps, garbage keeps the default.
        assert(Config::parse("notify_timeout = 999").notify_timeout == 1000);
        assert(Config::parse("notify_timeout = 90000").notify_timeout == 60000);
        assert(Config::parse("notify_timeout = soon").notify_timeout == 5000);

        const Config defaults = Config::parse("");
        assert(defaults.notifications);
        assert(defaults.notify_timeout == 5000);
        assert(defaults.notify_position == "top-right");
        assert(defaults.notify_history == 50);
    }

} // namespace

int main() {
    test_expiry();
    test_body_markup();
    test_actions();
    test_anchors();
    test_hint_bool();
    test_history();
    test_history_disabled();
    test_history_texture_cap();
    test_config();
    std::printf("notify tests passed\n");
    return 0;
}
