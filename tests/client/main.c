// fenriz-test: deliberately awkward Wayland clients, one subcommand per scenario.
//
//   fenriz-test popup                 run it once against $WAYLAND_DISPLAY
//   fenriz-test evil --loop 50        soak
//   fenriz-test destroy-parent --hold leave the surfaces up and look at them
//
// Exit 0 pass, 1 protocol error or lost compositor, 2 watchdog timeout.
#include "scenarios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE* out) {
    fprintf(out, "usage: fenriz-test <scenario> [--loop N] [--timeout S] [--hold] [--verbose]\n\n");
    for (const struct scenario* s = scenarios; s->name; s++)
        fprintf(out, "  %-15s %s\n", s->name, s->desc);
    fprintf(out, "\nexit: 0 pass, %d protocol error / compositor lost, %d timeout\n", WLC_EXIT_FAIL, WLC_EXIT_TIMEOUT);
}

int main(int argc, char** argv) {
    const char* name = NULL;
    int loop = 1, timeout = 30;

    // Also answer to fenriz-test-<scenario> when invoked through a symlink.
    const char* self = strrchr(argv[0], '/');
    self = self ? self + 1 : argv[0];
    if (!strncmp(self, "fenriz-test-", 12))
        name = self + 12;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--hold"))
            wlc_hold = true;
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            wlc_verbose = true;
        else if (!strcmp(a, "--loop") && i + 1 < argc)
            loop = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else if (!strcmp(a, "--list")) {
            for (const struct scenario* s = scenarios; s->name; s++)
                printf("%s\n", s->name);
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            return 1;
        } else
            name = a;
    }

    if (!name) {
        usage(stderr);
        return 1;
    }
    const struct scenario* found = NULL;
    for (const struct scenario* s = scenarios; s->name; s++)
        if (!strcmp(s->name, name))
            found = s;
    if (!found) {
        fprintf(stderr, "no such scenario: %s\n\n", name);
        usage(stderr);
        return 1;
    }

    wlc_watchdog(timeout * (loop > 1 ? loop : 1));

    struct wlc* c = wlc_connect();
    for (int i = 0; i < loop; i++) {
        if (loop > 1)
            fprintf(stderr, "== %s: iteration %d/%d\n", found->name, i + 1, loop);
        found->fn(c);
    }
    wlc_finish(c);
    printf("ok: %s\n", found->name);
    return 0;
}
