#pragma once

#include <algorithm>
#include <cmath>

namespace fenriz::bar {

    // A damped spring chasing `target`. Underdamped by default, so a morph overshoots a touch and settles.
    struct Spring {
        double value = 0;
        double velocity = 0;
        double target = 0;
        double stiffness = 380;
        double damping_ratio = 0.78;

        // Integrates in fixed substeps: a long frame (a stall, a resumed screen) must not blow the spring up.
        void step(double seconds) {
            constexpr double SUBSTEP = 1.0 / 240;
            const double damping = 2 * damping_ratio * std::sqrt(stiffness);
            seconds = std::min(seconds, 0.25); // past this the animation is lost anyway
            while (seconds > 0) {
                const double dt = std::min(seconds, SUBSTEP);
                velocity += (stiffness * (target - value) - damping * velocity) * dt;
                value += velocity * dt;
                seconds -= dt;
            }
        }

        // Within half a pixel and barely moving.
        bool settled() const { return std::abs(target - value) < 0.5 && std::abs(velocity) < 0.5; }

        void snap() {
            value = target;
            velocity = 0;
        }
    };

} // namespace fenriz::bar
