#include "notify.hpp"
#include "toast.hpp"

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

    void test_config() {
        const Config cfg = Config::parse("notifications = off\n"
                                         "notify_timeout = 12000\n"
                                         "notify_position = bottom-left\n");
        assert(!cfg.notifications);
        assert(cfg.notify_timeout == 12000);
        assert(cfg.notify_position == "bottom-left");

        // Out of range clamps, garbage keeps the default.
        assert(Config::parse("notify_timeout = 999").notify_timeout == 1000);
        assert(Config::parse("notify_timeout = 90000").notify_timeout == 60000);
        assert(Config::parse("notify_timeout = soon").notify_timeout == 5000);

        const Config defaults = Config::parse("");
        assert(defaults.notifications);
        assert(defaults.notify_timeout == 5000);
        assert(defaults.notify_position == "top-right");
    }

} // namespace

int main() {
    test_expiry();
    test_body_markup();
    test_actions();
    test_anchors();
    test_config();
    std::printf("notify tests passed\n");
    return 0;
}
