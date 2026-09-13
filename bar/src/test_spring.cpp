#include <cassert>
#include <cmath>

#include "spring.hpp"

using fenriz::bar::Spring;

namespace {

    // Frame count to settle from `from` to `to` at `dt` per frame, or -1 if it never does.
    int frames_to_settle(Spring s, double to, double dt, double* peak) {
        s.target = to;
        *peak = s.value;
        for (int i = 0; i < 2000; i++) {
            s.step(dt);
            assert(std::isfinite(s.value) && std::isfinite(s.velocity));
            *peak = std::max(*peak, s.value);
            if (s.settled())
                return i + 1;
        }
        return -1;
    }

    void test_settles_quickly_with_a_small_overshoot() {
        Spring s;
        s.value = 30;
        double peak = 0;
        const int frames = frames_to_settle(s, 400, 1.0 / 60, &peak);
        assert(frames > 0 && frames < 60); // under a second at 60 fps
        assert(peak > 400 && peak < 440);  // visible bounce, not a wobble
    }

    // A stalled frame clock hands the tick a huge dt; the spring must neither explode nor jump past its target wildly.
    void test_long_frames_stay_stable() {
        Spring s;
        s.value = 30;
        double peak = 0;
        assert(frames_to_settle(s, 400, 2.0, &peak) > 0);
        assert(peak < 440);
    }

    void test_frame_rate_does_not_change_the_motion() {
        Spring a, b;
        a.value = b.value = 0;
        a.target = b.target = 100;
        for (int i = 0; i < 60; i++)
            a.step(1.0 / 60);
        for (int i = 0; i < 144; i++)
            b.step(1.0 / 144);
        assert(std::abs(a.value - b.value) < 0.5);
    }

    void test_snap() {
        Spring s;
        s.target = 12;
        s.velocity = 99;
        s.snap();
        assert(s.value == 12 && s.velocity == 0 && s.settled());
    }

} // namespace

int main() {
    test_settles_quickly_with_a_small_overshoot();
    test_long_frames_stay_stable();
    test_frame_rate_does_not_change_the_motion();
    test_snap();
    return 0;
}
