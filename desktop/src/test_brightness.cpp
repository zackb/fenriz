#include <cassert>

#include "brightness.hpp"

using fenriz::desktop::dim_target;
using fenriz::desktop::step_target;

static void steps() {
    // 5% of a typical 8-bit panel is 12 raw levels, up and down.
    assert(step_target(255, 100, 5) == 112);
    assert(step_target(255, 100, -5) == 88);

    // Never off, never past the top, however hard the key is held.
    assert(step_target(255, 5, -5) == 1);
    assert(step_target(255, 250, 5) == 255);
    assert(step_target(255, 1, -5) == 1);
    assert(step_target(255, 255, 5) == 255);

    // A step that rounds to zero still moves, or a small panel would be stuck.
    assert(step_target(10, 5, 5) == 6);
    assert(step_target(10, 5, -5) == 4);
    assert(step_target(255, 100, 0) == 100);

    // Same "no maximum means not adjustable" contract as dim_target.
    assert(step_target(0, 50, 5) == 0);
    assert(step_target(-1, 50, 5) == 0);
}

int main() {
    // Ordinary percentages of a typical 8-bit and a 16-bit panel.
    assert(dim_target(255, 10) == 25);
    assert(dim_target(65535, 10) == 6553);
    assert(dim_target(100, 100) == 100);

    // Never dark enough to look broken, however small the panel or the percentage.
    assert(dim_target(255, 1) == 2);
    assert(dim_target(10, 1) == 1);
    assert(dim_target(1, 50) == 1);

    // Out-of-range input is clamped, not wrapped: config is validated but the API is public.
    assert(dim_target(255, 0) == 2); // 0 would be a black screen
    assert(dim_target(255, -50) == 2);
    assert(dim_target(255, 500) == 255);

    // A device that reports no maximum is not dimmable; 0 tells the caller to skip it.
    assert(dim_target(0, 50) == 0);
    assert(dim_target(-1, 50) == 0);

    steps();
    return 0;
}
