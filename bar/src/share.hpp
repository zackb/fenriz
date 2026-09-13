#pragma once

#include <algorithm>

namespace fenriz::bar {

    // Width for one of two widgets sharing `total`: the shorter keeps its natural width, the longer takes the rest,
    // and both get half when both overflow.
    inline int flex_share(int self, int other, int total) {
        const int half = total / 2;
        if (self + other <= total)
            return self;
        if (other <= half)
            return std::max(0, total - other);
        if (self <= half)
            return self;
        return std::max(0, half);
    }

} // namespace fenriz::bar
