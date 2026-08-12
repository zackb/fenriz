#include <cassert>

#include "volume.hpp"

using fenriz::desktop::volume_percent;
using fenriz::desktop::volume_step;

static bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

int main() {
    // Ordinary steps, up and down.
    assert(near(volume_step(0.50, 5), 0.55));
    assert(near(volume_step(0.50, -5), 0.45));
    assert(near(volume_step(0.50, 0), 0.50));

    // Clamped at both ends: silence is reachable, overdriving the sink is not.
    assert(near(volume_step(0.02, -5), 0.0));
    assert(near(volume_step(0.0, -5), 0.0));
    assert(near(volume_step(0.98, 5), 1.0));
    assert(near(volume_step(1.0, 5), 1.0));

    // Percentages are what the OSD shows, so they round rather than truncate.
    assert(volume_percent(0.0) == 0);
    assert(volume_percent(1.0) == 100);
    assert(volume_percent(0.5) == 50);
    assert(volume_percent(0.455) == 46);
    // mixer-api reports cubic values a hair off exact (0.49996948 for a wpctl "0.50").
    assert(volume_percent(0.49996948055904039) == 50);

    // Out-of-range input is clamped, not wrapped: the API is public.
    assert(volume_percent(-0.5) == 0);
    assert(volume_percent(1.53) == 100);

    return 0;
}
