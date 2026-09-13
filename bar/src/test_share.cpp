#include <cassert>

#include "share.hpp"

using fenriz::bar::flex_share;

int main() {
    // both fit
    assert(flex_share(100, 200, 400) == 100);
    assert(flex_share(200, 100, 400) == 200);
    // short one keeps its width, long one takes the rest
    assert(flex_share(100, 500, 400) == 100);
    assert(flex_share(500, 100, 400) == 300);
    // both overflow: even halves
    assert(flex_share(300, 500, 400) == 200);
    assert(flex_share(500, 300, 400) == 200);
    // the other one alone
    assert(flex_share(500, 0, 400) == 400);
    // no room
    assert(flex_share(100, 100, 0) == 0);
    assert(flex_share(100, 100, -20) == 0);
}
