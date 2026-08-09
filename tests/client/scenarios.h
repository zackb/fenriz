#ifndef FENRIZ_SCENARIOS_H
#define FENRIZ_SCENARIOS_H

#include "wlclient.h"

struct scenario {
    const char* name;
    void (*fn)(struct wlc* c);
    const char* desc;
};

extern const struct scenario scenarios[];

#endif
