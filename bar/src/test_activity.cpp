#include <cassert>

#include "activity.hpp"

using fenriz::bar::ActivityQueue;

namespace {

    void test_idle_is_the_clock() {
        ActivityQueue q;
        assert(q.showing() == "clock");
        assert(q.expire() == "clock");
    }

    void test_higher_or_equal_priority_replaces() {
        ActivityQueue q;
        assert(q.push({"media", ActivityQueue::MEDIA}));
        assert(q.push({"osd", ActivityQueue::LEVEL}));
        assert(q.showing() == "osd");
        // a track change does not interrupt a held volume key
        assert(!q.push({"media", ActivityQueue::MEDIA}));
        assert(q.showing() == "osd");
        // repeated key presses keep re-taking it
        assert(q.push({"osd", ActivityQueue::LEVEL}));
        assert(q.expire() == "clock");
    }

    void test_sticky_waits_underneath() {
        ActivityQueue q;
        assert(q.push({"osd", ActivityQueue::LEVEL}));
        // low battery arrives while a level shows: it does not take the pill yet
        assert(!q.push({"battery", ActivityQueue::EVENT, true}));
        assert(q.showing() == "osd");
        assert(q.expire() == "battery");
        // timed activities still show over it, and it comes back after
        assert(q.push({"media", ActivityQueue::MEDIA}));
        assert(q.expire() == "battery");
        q.dismiss_sticky();
        assert(q.showing() == "clock");
    }

    void test_sticky_on_an_idle_pill_shows_at_once() {
        ActivityQueue q;
        assert(q.push({"battery", ActivityQueue::EVENT, true}));
        assert(q.showing() == "battery");
    }

} // namespace

int main() {
    test_idle_is_the_clock();
    test_higher_or_equal_priority_replaces();
    test_sticky_waits_underneath();
    test_sticky_on_an_idle_pill_shows_at_once();
    return 0;
}
