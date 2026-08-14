// One function per subcommand. Everything here maps real surfaces and paints real pixels,
// so the tiling tree, the scene graph and the SSD path are all live while it runs.
//
// Two flavours of rudeness live side by side:
//   - legal but obnoxious runs inline, and a protocol error is a FAILURE (a compositor must
//     not kill a client for anything the spec permits);
//   - an outright violation runs under wlc_abuse(), where the child is expected to die and
//     the assertion is only that the compositor is still serving afterwards.
#define _GNU_SOURCE
#include "scenarios.h"

#include "alpha-modifier-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "kde-blur-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-foreign-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-toplevel-tag-v1-client-protocol.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RED 0xffcc3333u
#define GREEN 0xff33cc55u
#define BLUE 0xff3355ccu
#define GREY 0xff808080u

// --- helpers --------------------------------------------------------------------------

static void unmap(struct win* w) {
    wl_surface_attach(w->surface, NULL, 0, 0);
    wl_surface_commit(w->surface);
}

// After a null-buffer commit the surface is unconfigured again, so a remap has to redo the
// whole initial-commit / configure / ack dance. Skipping that is unconfigured_buffer.
static void remap(struct win* w, uint32_t argb) {
    w->configured = false;
    w->acked = false;
    wl_surface_commit(w->surface);
    wlc_map(w, argb);
}

static int connect_unix(const char* path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Resolve the state socket the way fenrizctl does. Returns false when there is none.
static bool ipc_path(char* out, size_t n) {
    const char* sock = getenv("FENRIZ_SOCKET");
    if (sock) {
        snprintf(out, n, "%s", sock);
        return true;
    }
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    const char* disp = getenv("WAYLAND_DISPLAY");
    if (!xdg || !disp)
        return false;
    snprintf(out, n, "%s/fenriz-%s.sock", xdg, disp);
    return true;
}

// Talk to the compositor's own control socket, the same one fenrizctl uses. Lets a
// scenario drive output/lid churn without the runner having to coordinate.
static int ipc_connect(void) {
    char path[256];
    if (!ipc_path(path, sizeof path))
        return -1;
    return connect_unix(path);
}

// The read-only event feed. Nothing arrives until something happens, so connect before
// provoking the event you want to observe.
static int ipc_event_connect(void) {
    char path[256];
    const char* sock = getenv("FENRIZ_EVENT_SOCKET");
    if (sock)
        snprintf(path, sizeof path, "%s", sock);
    else {
        if (!ipc_path(path, sizeof path))
            return -1;
        // Same name as the state socket with .events in place of .sock.
        size_t len = strlen(path);
        if (len > 5 && !strcmp(path + len - 5, ".sock"))
            path[len - 5] = 0;
        snprintf(path + strlen(path), sizeof path - strlen(path), ".events");
    }
    return connect_unix(path);
}

static void ipc_send(const char* line) {
    int fd = ipc_connect();
    if (fd < 0)
        return;
    dprintf(fd, "%s\n", line);
    close(fd);
}

// Collect everything the event feed has to say, stopping once it goes quiet for `ms`.
// Bounded so a compositor that never publishes fails the assertion instead of hanging.
static void ipc_event_drain(int fd, char* buf, size_t n, int ms) {
    size_t used = 0;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    while (used + 1 < n && poll(&p, 1, ms) > 0) {
        ssize_t got = read(fd, buf + used, n - 1 - used);
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    buf[used] = 0;
}

static int count_substr(const char* hay, const char* needle) {
    int n = 0;
    for (const char* p = hay; (p = strstr(p, needle)); p += strlen(needle))
        n++;
    return n;
}

// Every connection is greeted with a state snapshot, so one read is a complete query.
static bool ipc_snapshot(char* buf, size_t n) {
    int fd = ipc_connect();
    if (fd < 0)
        return false;
    ssize_t got = read(fd, buf, n - 1);
    close(fd);
    if (got <= 0)
        return false;
    buf[got] = 0;
    return true;
}

// --- popup ----------------------------------------------------------------------------

static void s_popup(struct wlc* c) {
    wlc_phase("mapping popup parent");
    struct win* t = wlc_toplevel(c, 500, 400, "fenriz-test popup");
    wlc_map(t, BLUE);

    wlc_phase("mapping grabbing popup");
    struct win* p1 = wlc_popup(t, 60, 60, 220, 160, true);
    wlc_map(p1, GREEN);

    wlc_phase("mapping nested popup");
    struct win* p2 = wlc_popup(p1, 30, 30, 160, 110, false);
    wlc_map(p2, RED);

    wlc_hold_point(c);

    // A popup anchored far off the right/bottom edge has to be flipped or slid back on
    // screen. The unconstrain box is in the parent's *surface* coordinates, so a CSD
    // shadow margin getting counted twice shows up here as a popup that never appears.
    wlc_phase("mapping off-screen-anchored popup");
    struct win* p3 = wlc_popup(t, 6000, 6000, 200, 150, false);
    wlc_map(p3, GREY);
    wlc_destroy(p3);
    wlc_roundtrip(c);

    if (xdg_popup_get_version(p1->popup) >= 3) {
        wlc_phase("repositioning popup");
        for (int i = 0; i < 8; i++) {
            struct xdg_positioner* pos = xdg_wm_base_create_positioner(c->wm_base);
            xdg_positioner_set_size(pos, 220, 160);
            xdg_positioner_set_anchor_rect(pos, 40 + i * 30, 40 + i * 20, 1, 1);
            xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
            xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
            xdg_popup_reposition(p1->popup, pos, 1000 + i);
            xdg_positioner_destroy(pos);
            wlc_roundtrip(c);
            wlc_paint(p1, GREEN);
        }
    }

    wlc_phase("unmap/remap nested popup");
    for (int i = 0; i < 5; i++) {
        wlc_destroy(p2);
        wlc_roundtrip(c);
        p2 = wlc_popup(p1, 30, 30, 160, 110, false);
        wlc_map(p2, RED);
    }

    // Topmost first: the other order is not_the_topmost_popup, exercised under `grab`.
    wlc_destroy(p2);
    wlc_destroy(p1);
    wlc_destroy(t);
    wlc_roundtrip(c);
}

// --- layer-popup ----------------------------------------------------------------------

// A popup must land entirely inside its root layer surface, which here is the whole
// output. x/y are the popup's own configured position already translated into the
// root's coordinate space by the caller.
static void expect_on_screen(struct win* p, int x, int y, struct win* root, const char* what) {
    wlc_log("%s configured at %d,%d %dx%d inside %dx%d", what, x, y, p->width, p->height, root->width, root->height);
    if (x < 0 || y < 0 || x + p->width > root->width || y + p->height > root->height)
        wlc_die("%s not unconstrained: %d,%d %dx%d spills out of %dx%d",
                what,
                x,
                y,
                p->width,
                p->height,
                root->width,
                root->height);
}

static void s_layer_popup(struct wlc* c) {
    if (!c->layer_shell)
        wlc_die("compositor has no zwlr_layer_shell_v1");

    wlc_phase("mapping full-output layer surface");
    struct win* l = wlc_layer(c, "fenriz-test-layer", ZWLR_LAYER_SHELL_V1_LAYER_TOP);
    wlc_map(l, BLUE);
    if (l->width <= 0 || l->height <= 0)
        wlc_die("layer surface never got an output-sized configure"); // 0x0 + all anchors owes us one

    // The desktop's right-click menu: a popup on a layer surface, anchored where the
    // pointer was. Anchored near the bottom-right corner it does not fit, and the
    // compositor has to slide/flip it back on screen — layer-shell roots are a
    // different coordinate space from toplevel ones and were once skipped entirely.
    wlc_phase("mapping corner-anchored layer popup");
    struct win* p1 = wlc_popup(l, l->width - 10, l->height - 10, 220, 160, false);
    wlc_map(p1, GREEN);
    expect_on_screen(p1, p1->cfg_x, p1->cfg_y, l, "layer popup");

    // A submenu off that popup comes back through the xdg-shell new_popup path and has
    // to resolve to the same layer-surface root. Its configure is relative to its parent
    // popup, so add the parent's position to get back into root coords.
    wlc_phase("mapping nested layer popup");
    struct win* p2 = wlc_popup(p1, 200, 140, 180, 200, false);
    wlc_map(p2, RED);
    expect_on_screen(p2, p1->cfg_x + p2->cfg_x, p1->cfg_y + p2->cfg_y, l, "nested layer popup");

    wlc_hold_point(c);

    wlc_destroy(p2);
    wlc_destroy(p1);
    wlc_destroy(l);
    wlc_roundtrip(c);
}

// --- resize ---------------------------------------------------------------------------

static void s_resize(struct wlc* c) {
    wlc_phase("mapping resize victim");
    struct win* t = wlc_toplevel(c, 400, 300, "fenriz-test resize");
    wlc_map(t, BLUE);
    wlc_hold_point(c);

    // A client is entitled to pick its own size; the configure is a suggestion. Sizes that
    // contradict the last configure are legal and are exactly what breaks naive clipping.
    static const int sizes[][2] = {
        {1, 1},
        {7, 3},
        {800, 600},
        {4000, 3000},
        {321, 197},
        {2, 1000},
        {1000, 2},
        {17, 17},
    };
    wlc_phase("resize storm");
    for (int pass = 0; pass < 4; pass++) {
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            int w = sizes[i][0], h = sizes[i][1];
            xdg_surface_set_window_geometry(t->xdg_surface, 0, 0, w, h);
            t->width = w;
            t->height = h;
            wlc_paint_size(t, RED, w, h);
            wlc_pump(c, 0);
        }
    }

    // Back to something sane, honouring whatever the compositor last asked for.
    wlc_phase("settling to the configured size");
    wlc_roundtrip(c);
    xdg_surface_set_window_geometry(
        t->xdg_surface, 0, 0, t->cfg_width ? t->cfg_width : 400, t->cfg_height ? t->cfg_height : 300);
    wlc_paint(t, GREEN);
    wlc_roundtrip(c);

    // Interactive resize with no pointer down: the compositor should decline rather than
    // enter a grab it can never leave. Starting it twice must not wedge anything either.
    wlc_phase("interactive resize requests");
    for (int i = 0; i < 3; i++) {
        xdg_toplevel_resize(t->toplevel, c->seat, c->last_serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
        xdg_toplevel_resize(t->toplevel, c->seat, c->last_serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT);
        wlc_roundtrip(c);
        wlc_paint(t, BLUE);
    }

    wlc_phase("min/max size churn");
    for (int i = 0; i < 10; i++) {
        xdg_toplevel_set_min_size(t->toplevel, 100 + i, 80 + i);
        xdg_toplevel_set_max_size(t->toplevel, 900 - i, 700 - i);
        wl_surface_commit(t->surface);
        wlc_pump(c, 0);
    }
    xdg_toplevel_set_min_size(t->toplevel, 0, 0);
    xdg_toplevel_set_max_size(t->toplevel, 0, 0);
    wlc_paint(t, GREEN);

    wlc_destroy(t);
    wlc_roundtrip(c);
}

// --- fullscreen -------------------------------------------------------------------------

static void s_fullscreen(struct wlc* c) {
    wlc_phase("mapping fullscreen victim");
    struct win* t = wlc_toplevel(c, 400, 300, "fenriz-test fullscreen");
    wlc_map(t, BLUE);
    wlc_hold_point(c);

    wlc_phase("fullscreen toggle storm");
    for (int i = 0; i < 20; i++) {
        xdg_toplevel_set_fullscreen(t->toplevel, NULL);
        wlc_roundtrip(c);
        wlc_paint(t, RED);
        xdg_toplevel_unset_fullscreen(t->toplevel);
        wlc_roundtrip(c);
        wlc_paint(t, BLUE);
    }

    // No roundtrip in between: two state changes racing inside one configure cycle.
    wlc_phase("fullscreen without waiting for the configure");
    for (int i = 0; i < 20; i++) {
        xdg_toplevel_set_fullscreen(t->toplevel, NULL);
        xdg_toplevel_unset_fullscreen(t->toplevel);
        xdg_toplevel_set_maximized(t->toplevel);
        xdg_toplevel_set_fullscreen(t->toplevel, NULL);
        xdg_toplevel_unset_maximized(t->toplevel);
        wl_surface_commit(t->surface);
        wlc_pump(c, 0);
    }
    xdg_toplevel_unset_fullscreen(t->toplevel);
    wlc_roundtrip(c);
    wlc_paint(t, BLUE);

    wlc_phase("fullscreen per output");
    for (int i = 0; i < c->n_outputs; i++) {
        xdg_toplevel_set_fullscreen(t->toplevel, c->outputs[i]);
        wlc_roundtrip(c);
        wlc_paint(t, GREEN);
    }
    xdg_toplevel_unset_fullscreen(t->toplevel);
    wlc_roundtrip(c);
    wlc_paint(t, BLUE);

    wlc_phase("fullscreen with a live popup child");
    struct win* p = wlc_popup(t, 40, 40, 200, 140, true);
    wlc_map(p, GREEN);
    for (int i = 0; i < 5; i++) {
        xdg_toplevel_set_fullscreen(t->toplevel, NULL);
        wlc_roundtrip(c);
        wlc_paint(t, RED);
        xdg_toplevel_unset_fullscreen(t->toplevel);
        wlc_roundtrip(c);
        wlc_paint(t, BLUE);
    }
    wlc_destroy(p);
    wlc_destroy(t);
    wlc_roundtrip(c);

    // set_fullscreen before the surface has ever been configured: legal, and the compositor
    // has to remember it and apply it on the first map.
    wlc_phase("fullscreen requested before the first configure");
    struct win* early = wlc_toplevel(c, 400, 300, "fenriz-test fullscreen-early");
    xdg_toplevel_set_fullscreen(early->toplevel, NULL);
    wl_surface_commit(early->surface);
    wlc_map(early, GREEN);
    if (!early->fullscreen)
        wlc_log("note: pre-map set_fullscreen did not stick");
    wlc_destroy(early);
    wlc_roundtrip(c);
}

// --- dnd ------------------------------------------------------------------------------

static void src_target(void* d, struct wl_data_source* s, const char* mime) {
    (void)d;
    (void)s;
    wlc_log("data source target %s", mime ?: "(none)");
}
static void src_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd) {
    (void)d;
    (void)s;
    (void)mime;
    const char payload[] = "fenriz-test";
    ssize_t r = write(fd, payload, sizeof payload - 1);
    (void)r;
    close(fd);
}
static void src_cancelled(void* d, struct wl_data_source* s) {
    (void)d;
    (void)s;
    wlc_log("data source cancelled");
}
static void src_noop(void* d, struct wl_data_source* s) {
    (void)d;
    (void)s;
}
static void src_action(void* d, struct wl_data_source* s, uint32_t a) {
    (void)d;
    (void)s;
    (void)a;
}
static const struct wl_data_source_listener source_listener = {
    .target = src_target,
    .send = src_send,
    .cancelled = src_cancelled,
    .dnd_drop_performed = src_noop,
    .dnd_finished = src_noop,
    .action = src_action,
};

static void s_dnd(struct wlc* c) {
    if (!c->data_device) {
        wlc_log("no wl_data_device_manager; nothing to test");
        return;
    }
    wlc_phase("mapping drag source and target");
    struct win* a = wlc_toplevel(c, 400, 300, "fenriz-test dnd-source");
    wlc_map(a, BLUE);
    struct win* b = wlc_toplevel(c, 400, 300, "fenriz-test dnd-target");
    wlc_map(b, GREEN);
    wlc_hold_point(c);

    for (int round = 0; round < 5; round++) {
        wlc_phase("drag round %d: start", round);
        struct wl_data_source* src = wl_data_device_manager_create_data_source(c->ddm);
        wl_data_source_add_listener(src, &source_listener, NULL);
        wl_data_source_offer(src, "text/plain;charset=utf-8");
        wl_data_source_offer(src, "text/plain");
        if (wl_data_source_get_version(src) >= 3)
            wl_data_source_set_actions(src,
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

        struct wl_surface* icon = wl_compositor_create_surface(c->compositor);
        wl_surface_attach(icon, wlc_buffer(c, 64, 64, RED), 0, 0);
        wl_surface_damage_buffer(icon, 0, 0, 64, 64);
        wl_surface_commit(icon);

        wl_data_device_start_drag(c->data_device, src, a->surface, icon, c->last_serial);
        wlc_roundtrip(c);
        wlc_pump(c, 60);

        // Tear the drag down a different way each round; the icon surface and the seat grab
        // outlive the source unless someone is keeping track.
        switch (round % 3) {
        case 0:
            wlc_phase("drag round %d: destroy source mid-drag", round);
            wl_data_source_destroy(src);
            wlc_roundtrip(c);
            wl_surface_destroy(icon);
            break;
        case 1:
            wlc_phase("drag round %d: destroy icon mid-drag", round);
            wl_surface_destroy(icon);
            wlc_roundtrip(c);
            wl_data_source_destroy(src);
            break;
        default:
            wlc_phase("drag round %d: drop the selection instead", round);
            wl_data_device_set_selection(c->data_device, src, c->last_serial);
            wlc_roundtrip(c);
            wl_data_device_set_selection(c->data_device, NULL, c->last_serial);
            wl_data_source_destroy(src);
            wl_surface_destroy(icon);
            break;
        }
        wlc_roundtrip(c);
        wlc_paint(a, BLUE);
        wlc_paint(b, GREEN);
    }

    wlc_destroy(b);
    wlc_destroy(a);
    wlc_roundtrip(c);
}

// --- grab -----------------------------------------------------------------------------

static void abuse_destroy_middle_popup(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse not-topmost");
    wlc_map(t, BLUE);
    struct win* p1 = wlc_popup(t, 40, 40, 200, 150, true);
    wlc_map(p1, GREEN);
    struct win* p2 = wlc_popup(p1, 20, 20, 150, 100, false);
    wlc_map(p2, RED);
    // xdg_popup.destroy on a non-topmost popup: not_the_topmost_popup.
    xdg_popup_destroy(p1->popup);
    p1->popup = NULL;
}

static void abuse_grab_bogus_serial(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse bogus grab");
    wlc_map(t, BLUE);
    struct win* p = wlc_popup(t, 40, 40, 200, 150, false);
    xdg_popup_grab(p->popup, c->seat, 0xdeadbeef);
    wlc_roundtrip(c);
    wlc_map(p, GREEN);
}

static void abuse_grab_after_map(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse late grab");
    wlc_map(t, BLUE);
    struct win* p = wlc_popup(t, 40, 40, 200, 150, false);
    wlc_map(p, GREEN);
    // xdg_popup.grab after the popup is mapped: invalid_grab.
    xdg_popup_grab(p->popup, c->seat, c->last_serial);
}

static void s_grab(struct wlc* c) {
    wlc_phase("mapping grab parent");
    struct win* t = wlc_toplevel(c, 500, 400, "fenriz-test grab");
    wlc_map(t, BLUE);

    wlc_phase("nested grabbing popups");
    struct win* p1 = wlc_popup(t, 50, 50, 220, 160, true);
    wlc_map(p1, GREEN);
    struct win* p2 = wlc_popup(p1, 25, 25, 170, 120, true);
    wlc_map(p2, RED);
    struct win* p3 = wlc_popup(p2, 15, 15, 120, 90, true);
    wlc_map(p3, GREY);
    wlc_hold_point(c);

    // Unwind the grab stack top-down, remapping each level as we go, so the compositor has
    // to hand the grab back one layer at a time rather than dropping the whole chain.
    wlc_phase("unwinding the grab stack");
    for (int i = 0; i < 5; i++) {
        wlc_destroy(p3);
        wlc_roundtrip(c);
        p3 = wlc_popup(p2, 15, 15, 120, 90, true);
        wlc_map(p3, GREY);
    }
    wlc_destroy(p3);
    wlc_destroy(p2);
    wlc_destroy(p1);
    wlc_roundtrip(c);

    // A popup that never gets its grab, dismissed by the client rather than the compositor.
    wlc_phase("ungrabbed popup churn");
    for (int i = 0; i < 10; i++) {
        struct win* p = wlc_popup(t, 30, 30, 150, 110, i % 2 == 0);
        wlc_map(p, i % 2 ? GREEN : RED);
        wlc_destroy(p);
        wlc_roundtrip(c);
    }

    wlc_destroy(t);
    wlc_roundtrip(c);

    wlc_abuse(c, "destroy a non-topmost popup", abuse_destroy_middle_popup);
    wlc_abuse(c, "grab with a serial that was never issued", abuse_grab_bogus_serial);
    wlc_abuse(c, "grab after the popup is mapped", abuse_grab_after_map);
}

// --- subsurface -------------------------------------------------------------------------

struct sub {
    struct wl_surface* surface;
    struct wl_subsurface* sub;
};

static struct sub make_sub(struct wlc* c, struct wl_surface* parent, int x, int y, int w, int h, uint32_t argb) {
    struct sub s;
    s.surface = wl_compositor_create_surface(c->compositor);
    s.sub = wl_subcompositor_get_subsurface(c->subcompositor, s.surface, parent);
    wl_subsurface_set_position(s.sub, x, y);
    wl_surface_attach(s.surface, wlc_buffer(c, w, h, argb), 0, 0);
    wl_surface_damage_buffer(s.surface, 0, 0, w, h);
    wl_surface_commit(s.surface);
    return s;
}

static void s_subsurface(struct wlc* c) {
    if (!c->subcompositor)
        wlc_die("no wl_subcompositor");

    wlc_phase("mapping subsurface parent");
    struct win* t = wlc_toplevel(c, 500, 400, "fenriz-test subsurface");
    wlc_map(t, BLUE);

    wlc_phase("three levels of subsurface");
    struct sub a = make_sub(c, t->surface, 20, 20, 300, 240, GREEN);
    struct sub b = make_sub(c, a.surface, 20, 20, 200, 160, RED);
    struct sub d = make_sub(c, b.surface, 20, 20, 120, 90, GREY);
    wl_surface_commit(t->surface);
    wlc_roundtrip(c);
    wlc_hold_point(c);

    wlc_phase("sync/desync churn");
    for (int i = 0; i < 20; i++) {
        wl_subsurface_set_sync(a.sub);
        wl_subsurface_set_desync(b.sub);
        wl_subsurface_set_sync(d.sub);
        wl_surface_attach(a.surface, wlc_buffer(c, 300, 240, i % 2 ? GREEN : RED), 0, 0);
        wl_surface_damage_buffer(a.surface, 0, 0, 300, 240);
        wl_surface_commit(a.surface);
        wl_subsurface_set_desync(a.sub);
        wl_subsurface_set_sync(b.sub);
        wl_surface_commit(t->surface);
        wlc_pump(c, 0);
    }

    wlc_phase("restacking");
    for (int i = 0; i < 20; i++) {
        wl_subsurface_place_above(a.sub, t->surface);
        wl_subsurface_place_below(b.sub, a.surface);
        wl_subsurface_place_above(b.sub, a.surface);
        wl_surface_commit(t->surface);
        wlc_pump(c, 0);
    }

    wlc_phase("moving subsurfaces off the parent");
    for (int i = 0; i < 20; i++) {
        wl_subsurface_set_position(a.sub, -400 + i * 40, -300 + i * 30);
        wl_subsurface_set_position(b.sub, 5000, 5000);
        wl_surface_commit(a.surface);
        wl_surface_commit(t->surface);
        wlc_pump(c, 0);
    }
    wl_subsurface_set_position(a.sub, 20, 20);
    wl_subsurface_set_position(b.sub, 20, 20);
    wl_surface_commit(t->surface);
    wlc_roundtrip(c);

    // Destroying the middle wl_surface leaves its child subsurface orphaned but alive.
    // Committing an orphan is legal and must not resurrect it or crash anything.
    wlc_phase("orphaning a subsurface");
    wl_subsurface_destroy(b.sub);
    wl_surface_destroy(b.surface);
    wlc_roundtrip(c);
    wl_surface_commit(d.surface);
    wl_surface_commit(t->surface);
    wlc_roundtrip(c);

    // Now pull the whole toplevel out from under the surviving subsurfaces.
    wlc_phase("destroying the parent with live subsurfaces");
    wlc_destroy(t);
    wlc_roundtrip(c);
    wl_surface_commit(a.surface);
    wl_surface_commit(d.surface);
    wlc_roundtrip(c);

    wl_subsurface_destroy(d.sub);
    wl_surface_destroy(d.surface);
    wl_subsurface_destroy(a.sub);
    wl_surface_destroy(a.surface);
    wlc_roundtrip(c);
}

// --- scale ----------------------------------------------------------------------------

static void frac_preferred(void* data, struct wp_fractional_scale_v1* f, uint32_t scale) {
    (void)f;
    *(uint32_t*)data = scale;
    wlc_log("preferred fractional scale %u/120", scale);
}
static const struct wp_fractional_scale_v1_listener frac_listener = {.preferred_scale = frac_preferred};

static void abuse_bad_buffer_scale(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse buffer scale");
    wlc_map(t, BLUE);
    // 301 is not divisible by 3: wl_surface.invalid_size.
    wl_surface_set_buffer_scale(t->surface, 3);
    wl_surface_attach(t->surface, wlc_buffer(c, 400, 301, RED), 0, 0);
    wl_surface_commit(t->surface);
}

static void abuse_viewport_out_of_bounds(struct wlc* c) {
    if (!c->viewporter)
        return;
    struct win* t = wlc_toplevel(c, 400, 300, "abuse viewport");
    wlc_map(t, BLUE);
    struct wp_viewport* vp = wp_viewporter_get_viewport(c->viewporter, t->surface);
    // A source rectangle larger than the buffer: wp_viewport.out_of_buffer.
    wp_viewport_set_source(
        vp, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(9000), wl_fixed_from_int(9000));
    wp_viewport_set_destination(vp, 400, 300);
    wl_surface_attach(t->surface, wlc_buffer(c, 400, 300, RED), 0, 0);
    wl_surface_commit(t->surface);
}

static void s_scale(struct wlc* c) {
    wlc_phase("mapping scale victim");
    struct win* t = wlc_toplevel(c, 400, 300, "fenriz-test scale");
    wlc_map(t, BLUE);

    uint32_t preferred = 0;
    struct wp_fractional_scale_v1* frac = NULL;
    if (c->frac_scale) {
        frac = wp_fractional_scale_manager_v1_get_fractional_scale(c->frac_scale, t->surface);
        wp_fractional_scale_v1_add_listener(frac, &frac_listener, &preferred);
        wlc_roundtrip(c);
    }
    wlc_hold_point(c);

    // Integer buffer scales, changed between commits. The buffer has to keep matching the
    // scale or it's invalid_size, so both move together.
    wlc_phase("integer buffer-scale churn");
    for (int i = 0; i < 20; i++) {
        int s = 1 + (i % 3);
        wl_surface_set_buffer_scale(t->surface, s);
        wl_surface_attach(t->surface, wlc_buffer(c, 400 * s, 300 * s, i % 2 ? GREEN : RED), 0, 0);
        wl_surface_damage_buffer(t->surface, 0, 0, 400 * s, 300 * s);
        wl_surface_commit(t->surface);
        wlc_pump(c, 0);
    }
    wl_surface_set_buffer_scale(t->surface, 1);
    wlc_paint(t, BLUE);
    wlc_roundtrip(c);

    // Fractional scale via viewport: a buffer sized for the preferred scale, scaled down to
    // the logical size by wp_viewport. This is the path a real fractional-scale client uses.
    if (c->viewporter) {
        wlc_phase("fractional scale via viewport");
        struct wp_viewport* vp = wp_viewporter_get_viewport(c->viewporter, t->surface);
        for (int i = 0; i < 12; i++) {
            uint32_t num = preferred ? preferred : 120 + i * 15; // 120 = scale 1.0
            int bw = (400 * (int)num + 60) / 120, bh = (300 * (int)num + 60) / 120;
            wl_surface_set_buffer_scale(t->surface, 1);
            wl_surface_attach(t->surface, wlc_buffer(c, bw, bh, GREEN), 0, 0);
            wp_viewport_set_source(
                vp, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(bw), wl_fixed_from_int(bh));
            wp_viewport_set_destination(vp, 400, 300);
            wl_surface_damage_buffer(t->surface, 0, 0, bw, bh);
            wl_surface_commit(t->surface);
            wlc_pump(c, 0);
        }
        // Cropping: source rect strictly inside the buffer, stretched back to full size.
        wlc_phase("viewport crop and stretch");
        for (int i = 0; i < 12; i++) {
            wl_surface_attach(t->surface, wlc_buffer(c, 400, 300, RED), 0, 0);
            wp_viewport_set_source(vp,
                                   wl_fixed_from_int(10 + i),
                                   wl_fixed_from_int(10 + i),
                                   wl_fixed_from_int(100),
                                   wl_fixed_from_int(80));
            wp_viewport_set_destination(vp, 400 + i, 300 + i);
            wl_surface_damage_buffer(t->surface, 0, 0, 400, 300);
            wl_surface_commit(t->surface);
            wlc_pump(c, 0);
        }
        wp_viewport_set_source(
            vp, wl_fixed_from_int(-1), wl_fixed_from_int(-1), wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        wp_viewport_set_destination(vp, -1, -1);
        wp_viewport_destroy(vp);
        wlc_paint(t, BLUE);
        wlc_roundtrip(c);
    }

    if (frac)
        wp_fractional_scale_v1_destroy(frac);
    wlc_destroy(t);
    wlc_roundtrip(c);

    wlc_abuse(c, "buffer size not divisible by the buffer scale", abuse_bad_buffer_scale);
    wlc_abuse(c, "viewport source rect outside the buffer", abuse_viewport_out_of_bounds);
}

// --- destroy-parent ---------------------------------------------------------------------

static void abuse_xdg_surface_before_role(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse defunct role");
    wlc_map(t, BLUE);
    // xdg_surface.destroy before xdg_toplevel.destroy: defunct_role_object.
    xdg_surface_destroy(t->xdg_surface);
    t->xdg_surface = NULL;
}

// Tear the toplevel down completely
static void kill_parent(struct win* t) {
    if (t->deco)
        zxdg_toplevel_decoration_v1_destroy(t->deco);
    xdg_toplevel_destroy(t->toplevel);
    xdg_surface_destroy(t->xdg_surface);
    wl_surface_destroy(t->surface);
    free(t);
}

static void s_destroy_parent(struct wlc* c) {
    for (int round = 0; round < 5; round++) {
        wlc_phase("round %d: parent -> popup -> popup", round);
        struct win* t = wlc_toplevel(c, 500, 400, "fenriz-test destroy-parent");
        wlc_map(t, BLUE);
        struct win* p1 = wlc_popup(t, 50, 50, 220, 160, true);
        wlc_map(p1, GREEN);
        struct win* p2 = wlc_popup(p1, 25, 25, 170, 120, false);
        wlc_map(p2, RED);
        if (round == 0)
            wlc_hold_point(c);

        wlc_phase("round %d: destroying the parent under a live nested popup", round);
        kill_parent(t);
        wlc_roundtrip(c);

        // The popups are still ours to clean up, topmost first.
        wlc_phase("round %d: cleaning up the orphaned popups", round);
        wlc_destroy(p2);
        wlc_destroy(p1);
        wlc_roundtrip(c);
    }

    // Same shape, but the parent is only unmapped rather than destroyed, then brought back.
    wlc_phase("unmap the parent under live popups, then remap");
    struct win* t = wlc_toplevel(c, 500, 400, "fenriz-test destroy-parent unmap");
    wlc_map(t, BLUE);
    struct win* p1 = wlc_popup(t, 50, 50, 220, 160, true);
    wlc_map(p1, GREEN);
    struct win* p2 = wlc_popup(p1, 25, 25, 170, 120, false);
    wlc_map(p2, RED);
    unmap(t);
    wlc_roundtrip(c);
    wlc_destroy(p2);
    wlc_destroy(p1);
    remap(t, BLUE);
    wlc_destroy(t);
    wlc_roundtrip(c);

    // And the reverse order: kill the nested popup's own parent popup first, legally, by
    // destroying the topmost then the middle, while the toplevel stays put.
    wlc_phase("collapsing the popup chain from the top with the parent alive");
    struct win* t2 = wlc_toplevel(c, 500, 400, "fenriz-test destroy-parent chain");
    wlc_map(t2, BLUE);
    for (int i = 0; i < 5; i++) {
        struct win* a = wlc_popup(t2, 40, 40, 200, 150, true);
        wlc_map(a, GREEN);
        struct win* b = wlc_popup(a, 20, 20, 150, 110, false);
        wlc_map(b, RED);
        struct win* d = wlc_popup(b, 10, 10, 100, 80, false);
        wlc_map(d, GREY);
        wlc_destroy(d);
        wlc_destroy(b);
        wlc_destroy(a);
        wlc_roundtrip(c);
    }
    wlc_destroy(t2);
    wlc_roundtrip(c);

    wlc_abuse(c, "destroy xdg_surface before its role object", abuse_xdg_surface_before_role);
}

// --- hotplug ----------------------------------------------------------------------------

// Drives output and lid churn over the control socket while surfaces stay mapped. The
// wl_surface enter/leave bookkeeping in wlclient.c is the assertion: a double enter or a
// leave for an output we were never on aborts the run.
static void s_hotplug(struct wlc* c) {
    wlc_phase("mapping windows for output churn");
    struct win* a = wlc_toplevel(c, 400, 300, "fenriz-test hotplug-a");
    wlc_map(a, BLUE);
    struct win* b = wlc_toplevel(c, 400, 300, "fenriz-test hotplug-b");
    wlc_map(b, GREEN);
    wlc_hold_point(c);

    char snap[8192];
    char names[8][64];
    int n_names = 0;
    if (ipc_snapshot(snap, sizeof snap)) {
        // Output names come out of the snapshot so this works on both the headless
        // (HEADLESS-n) and nested wayland (WL-n) backends without hardcoding either.
        for (const char* p = strstr(snap, "\"outputs\""); p && n_names < 8;) {
            p = strstr(p, "\"name\":\"");
            if (!p)
                break;
            p += 8;
            const char* end = strchr(p, '"');
            if (!end || end - p >= 64)
                break;
            snprintf(names[n_names], sizeof names[0], "%.*s", (int)(end - p), p);
            n_names++;
            p = end;
        }
    }
    if (!n_names) {
        wlc_log("no control socket or no outputs in the snapshot; nothing to toggle");
        wlc_pump(c, 500);
        wlc_destroy(b);
        wlc_destroy(a);
        wlc_roundtrip(c);
        return;
    }
    wlc_log("outputs: %d, first %s", n_names, names[0]);

    char cmd[192];
    for (int cycle = 0; cycle < 4; cycle++) {
        // Never disable the last output; that isn't hotplug, that's a headless compositor.
        for (int i = 1; i < n_names; i++) {
            wlc_phase("cycle %d: disabling %s", cycle, names[i]);
            snprintf(cmd, sizeof cmd, "{\"cmd\":\"output\",\"name\":\"%s\",\"enabled\":false}", names[i]);
            ipc_send(cmd);
            wlc_pump(c, 250);
            wlc_paint(a, RED);
            wlc_paint(b, GREY);

            wlc_phase("cycle %d: re-enabling %s", cycle, names[i]);
            snprintf(cmd, sizeof cmd, "{\"cmd\":\"output\",\"name\":\"%s\",\"enabled\":true}", names[i]);
            ipc_send(cmd);
            wlc_pump(c, 250);
            wlc_paint(a, BLUE);
            wlc_paint(b, GREEN);
        }

        wlc_phase("cycle %d: lid close/open", cycle);
        ipc_send("{\"cmd\":\"lid\",\"closed\":true}");
        wlc_pump(c, 250);
        ipc_send("{\"cmd\":\"lid\",\"closed\":false}");
        wlc_pump(c, 250);

        wlc_phase("cycle %d: workspace churn across outputs", cycle);
        for (int ws = 1; ws <= 3; ws++) {
            snprintf(cmd, sizeof cmd, "{\"cmd\":\"workspace\",\"n\":%d}", ws);
            ipc_send(cmd);
            wlc_pump(c, 120);
        }
        ipc_send("{\"cmd\":\"workspace\",\"n\":1}");
        wlc_pump(c, 200);
    }

    if (a->n_entered == 0 && b->n_entered == 0)
        wlc_die("neither surface is on any output after the churn settled");

    wlc_destroy(b);
    wlc_destroy(a);
    wlc_roundtrip(c);
}

// --- evil -------------------------------------------------------------------------------

static void abuse_ack_unknown_serial(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse ack");
    wlc_wait_configure(t);
    // A serial that was never sent in a configure: xdg_surface.invalid_serial.
    xdg_surface_ack_configure(t->xdg_surface, 0x7fffffff);
}

static void abuse_double_ack(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse double ack");
    wlc_map(t, BLUE);
    uint32_t first = t->last_configure_serial;
    xdg_toplevel_set_maximized(t->toplevel);
    wl_surface_commit(t->surface);
    wlc_roundtrip(c);
    // Acking the newer configure retires the older one; going back to it is invalid_serial.
    wlc_ack(t, t->last_configure_serial);
    xdg_surface_ack_configure(t->xdg_surface, first);
}

static void abuse_buffer_before_configure(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse unconfigured");
    // Attach before the first configure has been acked: xdg_surface.unconfigured_buffer.
    wl_surface_attach(t->surface, wlc_buffer(c, 400, 300, RED), 0, 0);
    wl_surface_commit(t->surface);
}

static void abuse_zero_window_geometry(struct wlc* c) {
    struct win* t = wlc_toplevel(c, 400, 300, "abuse geometry");
    wlc_map(t, BLUE);
    // Zero width/height window geometry: xdg_surface.invalid_size.
    xdg_surface_set_window_geometry(t->xdg_surface, 0, 0, 0, 0);
    wl_surface_commit(t->surface);
}

static void s_evil(struct wlc* c) {
    wlc_phase("mapping the racer");
    struct win* t = wlc_toplevel(c, 400, 300, "fenriz-test evil");
    wlc_map(t, BLUE);
    wlc_hold_point(c);

    // Let configures pile up unacked, then ack a *stale* one. Acking the second-newest
    // serial is legal while it is still pending, and leaves the newest one outstanding —
    // the race a slow real client hits. Acking a serial that an earlier ack already
    // superseded is NOT legal (it's invalid_serial), hence the monotonic guard; the
    // illegal version lives in abuse_double_ack below.
    wlc_phase("acking stale serials while configures pile up");
    uint32_t acked_up_to = t->last_configure_serial;
    for (int i = 0; i < 40; i++) {
        xdg_toplevel_set_maximized(t->toplevel);
        wl_surface_commit(t->surface);
        xdg_toplevel_unset_maximized(t->toplevel);
        wl_surface_commit(t->surface);
        wl_display_flush(c->display);
        wlc_pump(c, 5); // give the configures a moment to arrive, but never block
        uint32_t stale = t->prev_configure_serial;
        if (stale > acked_up_to && stale != t->last_configure_serial) {
            xdg_surface_ack_configure(t->xdg_surface, stale);
            acked_up_to = stale;
            t->acked = true;
        }
        // Several commits between configures, none of them acked.
        wlc_paint_noack(t, i % 2 ? RED : GREEN);
        wlc_paint_noack(t, GREY);
        wlc_paint_noack(t, BLUE);
    }
    wlc_roundtrip(c);
    if (t->last_configure_serial > acked_up_to)
        wlc_ack(t, t->last_configure_serial);
    wlc_paint(t, BLUE);
    wlc_roundtrip(c);

    // Resize on every frame callback, with a buffer that contradicts the last configure.
    wlc_phase("self-resizing on every frame");
    for (int i = 0; i < 60; i++) {
        int w = 200 + (i * 37) % 600, h = 150 + (i * 53) % 400;
        xdg_surface_set_window_geometry(t->xdg_surface, 0, 0, w, h);
        t->width = w;
        t->height = h;
        wlc_paint_size(t, i % 2 ? GREEN : RED, w, h);
        wlc_pump(c, 0);
    }
    wlc_roundtrip(c);

    // Unmap with a null buffer and bring it straight back, repeatedly. The compositor has
    // to fully retract the view each time or the tiling tree grows phantom leaves.
    wlc_phase("null-buffer unmap/remap churn");
    for (int i = 0; i < 15; i++) {
        unmap(t);
        wlc_roundtrip(c);
        remap(t, i % 2 ? GREEN : BLUE);
    }

    // Toggle floating between unmaps, so the view crosses the tiling/floating boundary
    // while it's being torn down and rebuilt.
    wlc_phase("float/tile churn across unmaps");
    for (int i = 0; i < 10; i++) {
        ipc_send("{\"cmd\":\"dispatch\",\"action\":\"togglefloating\"}");
        wlc_pump(c, 40);
        unmap(t);
        wlc_roundtrip(c);
        remap(t, RED);
    }
    wlc_destroy(t);
    wlc_roundtrip(c);

    // Destroy and recreate the whole role object as fast as the socket allows.
    wlc_phase("destroy/recreate the toplevel in a tight loop");
    for (int i = 0; i < 40; i++) {
        struct win* w = wlc_toplevel(c, 300 + i, 200 + i, "fenriz-test evil churn");
        wlc_map(w, i % 2 ? RED : GREEN);
        if (i % 3 == 0) {
            struct win* p = wlc_popup(w, 20, 20, 120, 90, true);
            wl_surface_commit(p->surface);
            // Destroy the popup before it was ever configured or painted.
            wlc_destroy(p);
        }
        wlc_destroy(w);
    }
    wlc_roundtrip(c);

    // Create surfaces and drop them without ever committing, then flush all at once.
    wlc_phase("create and abandon surfaces without committing");
    for (int i = 0; i < 40; i++) {
        struct wl_surface* s = wl_compositor_create_surface(c->compositor);
        struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(c->wm_base, s);
        struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
        xdg_toplevel_destroy(tl);
        xdg_surface_destroy(xs);
        wl_surface_destroy(s);
    }
    wlc_roundtrip(c);

    wlc_abuse(c, "ack a serial that was never sent", abuse_ack_unknown_serial);
    wlc_abuse(c, "ack a serial an earlier ack already retired", abuse_double_ack);
    wlc_abuse(c, "attach a buffer before the first configure", abuse_buffer_before_configure);
    wlc_abuse(c, "zero-sized window geometry", abuse_zero_window_geometry);
}

// --- table ------------------------------------------------------------------------------

// --- workspace ------------------------------------------------------------------------
//
// ext-workspace-v1: what a bar binds to list workspaces and click one to switch. The oracle
// runs both ways — the protocol's view has to agree with the control socket, an `activate`
// request has to actually move the compositor, and a switch made behind the protocol's back
// (over the socket, as a keybind would) has to come back out as a state event.

#define WS_MAX_TEST 40

static struct ws_entry {
    struct ext_workspace_handle_v1* handle;
    char name[32];
    uint32_t state;
    uint32_t caps;
    bool removed;
} ws_list[WS_MAX_TEST];

static struct ext_workspace_manager_v1* ws_manager;
static int n_ws;
static int ws_groups;
static int ws_dones;

static struct ws_entry* ws_of(struct ext_workspace_handle_v1* h) {
    for (int i = 0; i < n_ws; i++)
        if (ws_list[i].handle == h)
            return &ws_list[i];
    wlc_die("state for an ext_workspace_handle_v1 we were never told about");
    return NULL;
}

static struct ws_entry* ws_named(const char* name) {
    for (int i = 0; i < n_ws; i++)
        if (!ws_list[i].removed && !strcmp(ws_list[i].name, name))
            return &ws_list[i];
    return NULL;
}

static void ws_h_id(void* d, struct ext_workspace_handle_v1* h, const char* id) {
    (void)d;
    (void)h;
    (void)id;
}
static void ws_h_name(void* d, struct ext_workspace_handle_v1* h, const char* name) {
    (void)d;
    snprintf(ws_of(h)->name, sizeof ws_list[0].name, "%s", name);
}
static void ws_h_coordinates(void* d, struct ext_workspace_handle_v1* h, struct wl_array* coords) {
    (void)d;
    (void)h;
    (void)coords;
}
static void ws_h_state(void* d, struct ext_workspace_handle_v1* h, uint32_t state) {
    (void)d;
    struct ws_entry* e = ws_of(h);
    e->state = state;
    wlc_log("workspace %s state=%u", e->name, state);
}
static void ws_h_capabilities(void* d, struct ext_workspace_handle_v1* h, uint32_t caps) {
    (void)d;
    ws_of(h)->caps = caps;
}
static void ws_h_removed(void* d, struct ext_workspace_handle_v1* h) {
    (void)d;
    ws_of(h)->removed = true;
}
static const struct ext_workspace_handle_v1_listener ws_handle_listener = {
    .id = ws_h_id,
    .name = ws_h_name,
    .coordinates = ws_h_coordinates,
    .state = ws_h_state,
    .capabilities = ws_h_capabilities,
    .removed = ws_h_removed,
};

static uint32_t ws_group_caps;
static void ws_g_capabilities(void* d, struct ext_workspace_group_handle_v1* g, uint32_t caps) {
    (void)d;
    (void)g;
    ws_group_caps = caps;
    wlc_log("workspace group caps=%u", caps);
}
static void ws_g_output_enter(void* d, struct ext_workspace_group_handle_v1* g, struct wl_output* o) {
    (void)d;
    (void)g;
    (void)o;
}
static void ws_g_output_leave(void* d, struct ext_workspace_group_handle_v1* g, struct wl_output* o) {
    (void)d;
    (void)g;
    (void)o;
}
static void ws_g_enter(void* d, struct ext_workspace_group_handle_v1* g, struct ext_workspace_handle_v1* w) {
    (void)d;
    (void)g;
    (void)w;
}
static void ws_g_leave(void* d, struct ext_workspace_group_handle_v1* g, struct ext_workspace_handle_v1* w) {
    (void)d;
    (void)g;
    (void)w;
}
static void ws_g_removed(void* d, struct ext_workspace_group_handle_v1* g) {
    (void)d;
    (void)g;
    ws_groups--;
}
static const struct ext_workspace_group_handle_v1_listener ws_group_listener = {
    .capabilities = ws_g_capabilities,
    .output_enter = ws_g_output_enter,
    .output_leave = ws_g_output_leave,
    .workspace_enter = ws_g_enter,
    .workspace_leave = ws_g_leave,
    .removed = ws_g_removed,
};

static void ws_m_group(void* d, struct ext_workspace_manager_v1* m, struct ext_workspace_group_handle_v1* g) {
    (void)d;
    (void)m;
    ws_groups++;
    ext_workspace_group_handle_v1_add_listener(g, &ws_group_listener, NULL);
}
static void ws_m_workspace(void* d, struct ext_workspace_manager_v1* m, struct ext_workspace_handle_v1* w) {
    (void)d;
    (void)m;
    if (n_ws == WS_MAX_TEST)
        wlc_die("more than %d workspaces advertised", WS_MAX_TEST);
    ws_list[n_ws++] = (struct ws_entry){.handle = w};
    ext_workspace_handle_v1_add_listener(w, &ws_handle_listener, NULL);
}
static void ws_m_done(void* d, struct ext_workspace_manager_v1* m) {
    (void)d;
    (void)m;
    ws_dones++;
}
static void ws_m_finished(void* d, struct ext_workspace_manager_v1* m) {
    (void)d;
    (void)m;
    wlc_die("compositor sent finished without being asked to stop");
}
static const struct ext_workspace_manager_v1_listener ws_manager_listener = {
    .workspace_group = ws_m_group,
    .workspace = ws_m_workspace,
    .done = ws_m_done,
    .finished = ws_m_finished,
};

static void ws_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (strcmp(iface, ext_workspace_manager_v1_interface.name))
        return;
    ws_manager = wl_registry_bind(reg, name, &ext_workspace_manager_v1_interface, 1);
    // The handles and their initial state follow immediately on bind, so the listener has
    // to go on here rather than after the roundtrip that would already have drained them.
    ext_workspace_manager_v1_add_listener(ws_manager, &ws_manager_listener, NULL);
}
static void ws_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener ws_registry_listener = {
    .global = ws_registry_global,
    .global_remove = ws_registry_remove,
};

// The compositor's own idea of the active workspace, as a bar would read it off the socket.
static int ws_ipc_active(void) {
    char snap[8192];
    if (!ipc_snapshot(snap, sizeof snap))
        return 0;
    const char* p = strstr(snap, "\"workspaces\":{\"active\":");
    return p ? atoi(p + 23) : 0;
}

static const char* ws_want_active;
static bool ws_is_active(void* arg) {
    (void)arg;
    struct ws_entry* e = ws_named(ws_want_active);
    return e && (e->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE);
}

static void s_workspace(struct wlc* c) {
    ws_manager = NULL;
    n_ws = ws_groups = ws_dones = 0;
    ws_group_caps = 0;

    wlc_phase("binding ext_workspace_manager_v1");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &ws_registry_listener, NULL);
    wlc_roundtrip(c); // globals, and the bind above
    if (!ws_manager)
        wlc_die("compositor does not advertise ext_workspace_manager_v1");
    wlc_roundtrip(c); // the workspaces, their state, and the closing done

    if (!ws_dones)
        wlc_die("no done event after the initial workspace burst");
    if (n_ws < 3)
        wlc_die("expected at least 3 workspaces, got %d", n_ws);
    if (ws_groups < 1)
        wlc_die("workspaces advertised with no workspace group");
    if (ws_group_caps & EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE)
        wlc_die("group claims create_workspace; the set is fixed at `workspaces = N`");

    // A bar needs a name to render and the activate cap to be clickable at all.
    int active_seen = 0;
    for (int i = 0; i < n_ws; i++) {
        char want[32];
        snprintf(want, sizeof want, "%d", i + 1);
        if (strcmp(ws_list[i].name, want))
            wlc_die("workspace %d is named \"%s\", expected \"%s\"", i, ws_list[i].name, want);
        if (!(ws_list[i].caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE))
            wlc_die("workspace %s can't be activated; a bar could not click it", ws_list[i].name);
        if (ws_list[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
            active_seen++;
    }
    if (!active_seen)
        wlc_die("no workspace is active");
    wlc_log("%d workspaces, %d group(s), %d active", n_ws, ws_groups, active_seen);

    // An empty workspace nobody is on is hidden, so a bar lists only the ones worth drawing.
    // Nothing of ours is mapped yet, so everything but the shown ones should be hidden.
    int hidden = 0;
    for (int i = 0; i < n_ws; i++) {
        bool is_hidden = ws_list[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN;
        if (is_hidden && (ws_list[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE))
            wlc_die("workspace %s is active and hidden at once", ws_list[i].name);
        hidden += is_hidden;
    }
    if (n_ws > active_seen && !hidden)
        wlc_die("%d workspaces, %d shown, none hidden — a bar would list every empty one", n_ws, active_seen);
    wlc_log("%d hidden", hidden);

    // A window so the switch has something to lay out and the tiling path runs too.
    struct win* w = wlc_toplevel(c, 400, 300, "fenriz-test workspace");
    wlc_map(w, BLUE);
    wlc_hold_point(c);

    // Client -> compositor: this is the click.
    wlc_phase("activating workspace 3");
    ext_workspace_handle_v1_activate(ws_named("3")->handle);
    ext_workspace_manager_v1_commit(ws_manager);
    ws_want_active = "3";
    wlc_until(c, ws_is_active, NULL);
    if (ws_ipc_active() != 3)
        wlc_die("protocol says workspace 3 is active, the control socket says %d", ws_ipc_active());
    // Our toplevel is still on workspace 1. Holding a window is enough to stay listed, even
    // once no screen is showing it — occupancy, not visibility, is what `hidden` tracks.
    if (ws_named("1")->state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN)
        wlc_die("workspace 1 is hidden with a mapped window on it");

    // Compositor -> client: the same switch made the way a keybind makes it.
    wlc_phase("switching back to workspace 1 over the control socket");
    ipc_send("{\"cmd\":\"workspace\",\"n\":1}");
    ws_want_active = "1";
    wlc_until(c, ws_is_active, NULL);
    if (ws_ipc_active() != 1)
        wlc_die("protocol says workspace 1 is active, the control socket says %d", ws_ipc_active());

    // One workspace is shown per screen, so the count can't drift as they're switched —
    // a workspace that never has its active bit cleared would show up here.
    int still_active = 0;
    for (int i = 0; i < n_ws; i++)
        if (ws_list[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
            still_active++;
    if (still_active != active_seen)
        wlc_die("%d workspaces active after switching, %d before", still_active, active_seen);

    // Requests we never advertise a capability for must be ignored, not fatal.
    wlc_phase("sending requests the compositor has no capability for");
    ext_workspace_handle_v1_deactivate(ws_named("1")->handle);
    ext_workspace_handle_v1_remove(ws_named("2")->handle);
    ext_workspace_manager_v1_commit(ws_manager);
    wlc_pump(c, 300);
    if (!ws_named("2"))
        wlc_die("workspace 2 was removed despite no remove capability");

    wlc_destroy(w);
    wlc_roundtrip(c);
}

// --- foreign ----------------------------------------------------------------------------
//
// xdg-foreign: one client exports a toplevel handle, another imports it and declares its own
// window a child. This is how a portal's file chooser is parented to the app that opened it.
// The oracle is not "the global exists" — it is that the parent relationship reaches the
// compositor's window rules, which float anything with a parent. Read back over the control
// socket, since a client cannot see its own tiling state.

static struct zxdg_exporter_v2* fe_exporter;
static struct zxdg_importer_v2* fe_importer;
static struct zxdg_exporter_v1* fe_exporter_v1;
static struct zxdg_importer_v1* fe_importer_v1;
static char fe_handle[256];

static void fe_handle_cb(void* d, struct zxdg_exported_v2* e, const char* handle) {
    (void)d;
    (void)e;
    snprintf(fe_handle, sizeof fe_handle, "%s", handle);
    wlc_log("exported handle: %s", handle);
}
static const struct zxdg_exported_v2_listener fe_exported_listener = {.handle = fe_handle_cb};

static void fe_destroyed(void* d, struct zxdg_imported_v2* i) {
    (void)d;
    (void)i;
    wlc_log("imported handle was revoked");
}
static const struct zxdg_imported_v2_listener fe_imported_listener = {.destroyed = fe_destroyed};

static void fe_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, zxdg_exporter_v2_interface.name))
        fe_exporter = wl_registry_bind(reg, name, &zxdg_exporter_v2_interface, 1);
    else if (!strcmp(iface, zxdg_importer_v2_interface.name))
        fe_importer = wl_registry_bind(reg, name, &zxdg_importer_v2_interface, 1);
    else if (!strcmp(iface, zxdg_exporter_v1_interface.name))
        fe_exporter_v1 = wl_registry_bind(reg, name, &zxdg_exporter_v1_interface, 1);
    else if (!strcmp(iface, zxdg_importer_v1_interface.name))
        fe_importer_v1 = wl_registry_bind(reg, name, &zxdg_importer_v1_interface, 1);
}
static void fe_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener fe_registry_listener = {
    .global = fe_registry_global,
    .global_remove = fe_registry_remove,
};

static bool fe_have_handle(void* arg) {
    (void)arg;
    return fe_handle[0] != 0;
}

// Is the window with this title floating, per the compositor's own state feed?
static bool fe_is_floating(const char* title) {
    char snap[16384];
    if (!ipc_snapshot(snap, sizeof snap))
        wlc_die("no control socket; cannot tell whether the child floated");
    const char* p = strstr(snap, title);
    if (!p)
        wlc_die("window \"%s\" is not in the state snapshot", title);
    // Fields run appId, title, workspace, floating: read the first `floating` after the title.
    const char* f = strstr(p, "\"floating\":");
    return f && !strncmp(f + 11, "true", 4);
}

static void s_foreign(struct wlc* c) {
    fe_exporter = NULL;
    fe_importer = NULL;
    fe_exporter_v1 = NULL;
    fe_importer_v1 = NULL;
    fe_handle[0] = 0;

    wlc_phase("binding xdg-foreign");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &fe_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!fe_exporter || !fe_importer)
        wlc_die("compositor does not advertise zxdg_exporter_v2 / zxdg_importer_v2");
    if (!fe_exporter_v1 || !fe_importer_v1)
        wlc_die("compositor does not advertise the v1 pair (toolkits still bind it)");

    // The parent has to be mapped before it can be exported: wlroots drops a set_parent
    // against a surface that has no role yet, and the whole point here is the parent link.
    wlc_phase("mapping the exporting toplevel");
    struct win* parent = wlc_toplevel(c, 600, 400, "fenriz-test foreign-parent");
    wlc_map(parent, BLUE);

    wlc_phase("exporting it");
    struct zxdg_exported_v2* exported = zxdg_exporter_v2_export_toplevel(fe_exporter, parent->surface);
    zxdg_exported_v2_add_listener(exported, &fe_exported_listener, NULL);
    wlc_until(c, fe_have_handle, NULL);

    wlc_phase("importing the handle and parenting a second toplevel to it");
    struct zxdg_imported_v2* imported = zxdg_importer_v2_import_toplevel(fe_importer, fe_handle);
    zxdg_imported_v2_add_listener(imported, &fe_imported_listener, NULL);

    // set_parent_of before the child's first buffer: window rules run when it maps, and a
    // parent arriving afterwards would not retroactively float it.
    struct win* child = wlc_toplevel(c, 300, 200, "fenriz-test foreign-child");
    zxdg_imported_v2_set_parent_of(imported, child->surface);
    wlc_roundtrip(c);
    wlc_map(child, GREEN);
    wlc_roundtrip(c);
    wlc_hold_point(c);

    if (!fe_is_floating("fenriz-test foreign-child"))
        wlc_die("the imported child tiled; the parent link never reached the window rules");
    // Control: the same client, same app_id, no parent — proves the float above came from
    // xdg-foreign and not from some blanket rule about this test's windows.
    if (fe_is_floating("fenriz-test foreign-parent"))
        wlc_die("the exporting parent floated too; the float is not coming from the parent link");
    wlc_log("child floated, parent tiled");

    // Revoking the export must not take the child or the compositor with it.
    wlc_phase("destroying the export under a live import");
    zxdg_exported_v2_destroy(exported);
    wlc_roundtrip(c);
    wlc_pump(c, 200);

    zxdg_imported_v2_destroy(imported);
    wlc_destroy(child);
    wlc_destroy(parent);
    wlc_roundtrip(c);
}

// --- dialog -----------------------------------------------------------------------------
//
// xdg-dialog-v1. The dialog hint alone changes nothing — the protocol has no effect without
// a parent toplevel, and a parented window already floats. `modal` is the new information:
// focus must not leave a modal dialog for its parent. The oracle is the compositor's own
// activeWindow, driven by asking it to cycle focus over the control socket, and the control
// is unset_modal on the same window — one bit, everything else held still.

static struct xdg_wm_dialog_v1* fd_wm_dialog;

static void fd_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, xdg_wm_dialog_v1_interface.name))
        fd_wm_dialog = wl_registry_bind(reg, name, &xdg_wm_dialog_v1_interface, 1);
}
static void fd_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener fd_registry_listener = {
    .global = fd_registry_global,
    .global_remove = fd_registry_remove,
};

// The focused window's title, per the compositor. Empty when nothing is focused.
static void fd_active_title(char* out, size_t n) {
    char snap[16384];
    out[0] = 0;
    if (!ipc_snapshot(snap, sizeof snap))
        wlc_die("no control socket; cannot tell what is focused");
    const char* p = strstr(snap, "\"activeWindow\":");
    if (!p)
        return;
    const char* t = strstr(p, "\"title\":\"");
    if (!t)
        return; // activeWindow: null
    t += 9;
    const char* end = strchr(t, '"');
    if (!end || (size_t)(end - t) >= n)
        return;
    snprintf(out, n, "%.*s", (int)(end - t), t);
}

static void s_dialog(struct wlc* c) {
    fd_wm_dialog = NULL;

    wlc_phase("binding xdg_wm_dialog_v1");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &fd_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!fd_wm_dialog)
        wlc_die("compositor does not advertise xdg_wm_dialog_v1");

    // The parent must be mapped before set_parent: wlroots drops a parent that has no role yet.
    wlc_phase("mapping the parent");
    struct win* parent = wlc_toplevel(c, 600, 400, "fenriz-test dialog-parent");
    wlc_map(parent, BLUE);

    wlc_phase("mapping a modal dialog on it");
    struct win* dlg = wlc_toplevel(c, 300, 200, "fenriz-test dialog-modal");
    xdg_toplevel_set_parent(dlg->toplevel, parent->toplevel);
    struct xdg_dialog_v1* dialog = xdg_wm_dialog_v1_get_xdg_dialog(fd_wm_dialog, dlg->toplevel);
    xdg_dialog_v1_set_modal(dialog);
    wlc_map(dlg, GREEN);
    wlc_roundtrip(c);

    char title[256];
    fd_active_title(title, sizeof title);
    if (strcmp(title, "fenriz-test dialog-modal"))
        wlc_die("the modal dialog did not take focus when it mapped (focused: \"%s\")", title);

    // Cycling focus must never land on the parent while the dialog is modal. Twice: with two
    // windows a single cycle could sit still for the wrong reason.
    wlc_phase("cycling focus with the dialog modal");
    for (int i = 0; i < 4; i++) {
        ipc_send("{\"cmd\":\"dispatch\",\"action\":\"focusnext\"}");
        wlc_pump(c, 150);
        fd_active_title(title, sizeof title);
        if (strcmp(title, "fenriz-test dialog-modal"))
            wlc_die("focus reached \"%s\" on cycle %d; a modal dialog must hold it", title, i);
    }

    // Same two windows, same stacking — drop only the modal bit and focus must move again.
    wlc_phase("unsetting modal; focus must reach the parent now");
    xdg_dialog_v1_unset_modal(dialog);
    wlc_roundtrip(c);
    bool reached_parent = false;
    for (int i = 0; i < 4 && !reached_parent; i++) {
        ipc_send("{\"cmd\":\"dispatch\",\"action\":\"focusnext\"}");
        wlc_pump(c, 150);
        fd_active_title(title, sizeof title);
        reached_parent = !strcmp(title, "fenriz-test dialog-parent");
    }
    if (!reached_parent)
        wlc_die("focus never reached the parent after unset_modal; the test proves nothing");
    wlc_log("modal held focus, unset_modal released it");

    wlc_hold_point(c);

    // Destroying the dialog object must unapply the hint, not strand focus.
    xdg_dialog_v1_destroy(dialog);
    wlc_roundtrip(c);
    wlc_destroy(dlg);
    wlc_destroy(parent);
    wlc_roundtrip(c);
}

// --- icon -------------------------------------------------------------------------------
//
// xdg-toplevel-icon-v1. fenriz keeps the icon *name* — an XDG icon-theme name, the thing a
// bar can actually resolve — and publishes it on the control socket. Pixel buffers are
// dropped, so this checks the name round-trips, that a buffer-only icon reports no name
// rather than a stale one, and that unsetting clears it.

static struct xdg_toplevel_icon_manager_v1* fi_manager;

static void fi_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name))
        fi_manager = wl_registry_bind(reg, name, &xdg_toplevel_icon_manager_v1_interface, 1);
}
static void fi_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener fi_registry_listener = {
    .global = fi_registry_global,
    .global_remove = fi_registry_remove,
};

// The `icon` the compositor publishes for the window with this title.
static void fi_icon_of(const char* title, char* out, size_t n) {
    char snap[16384];
    out[0] = 0;
    if (!ipc_snapshot(snap, sizeof snap))
        wlc_die("no control socket; cannot read the published icon");
    const char* p = strstr(snap, title);
    if (!p)
        wlc_die("window \"%s\" is not in the state snapshot", title);
    const char* i = strstr(p, "\"icon\":\"");
    if (!i)
        wlc_die("no icon field in the snapshot; the feed never grew one");
    i += 8;
    const char* end = strchr(i, '"');
    if (!end || (size_t)(end - i) >= n)
        return;
    snprintf(out, n, "%.*s", (int)(end - i), i);
}

static void s_icon(struct wlc* c) {
    fi_manager = NULL;

    wlc_phase("binding xdg_toplevel_icon_manager_v1");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &fi_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!fi_manager)
        wlc_die("compositor does not advertise xdg_toplevel_icon_manager_v1");

    struct win* w = wlc_toplevel(c, 400, 300, "fenriz-test icon-window");
    wlc_map(w, BLUE);

    char icon[256];
    fi_icon_of("fenriz-test icon-window", icon, sizeof icon);
    if (icon[0])
        wlc_die("a window that set no icon reports \"%s\"", icon);

    // Nothing in the protocol says the icon has to wait for the window to map, and a toolkit
    // that sets it up front is doing the normal thing.
    wlc_phase("setting an icon before the window maps");
    struct win* early = wlc_toplevel(c, 300, 200, "fenriz-test icon-early");
    struct xdg_toplevel_icon_v1* early_icon = xdg_toplevel_icon_manager_v1_create_icon(fi_manager);
    xdg_toplevel_icon_v1_set_name(early_icon, "web-browser");
    xdg_toplevel_icon_manager_v1_set_icon(fi_manager, early->toplevel, early_icon);
    wlc_map(early, GREEN);
    wlc_pump(c, 150);
    fi_icon_of("fenriz-test icon-early", icon, sizeof icon);
    if (strcmp(icon, "web-browser"))
        wlc_die("icon set before map did not reach the feed (got \"%s\")", icon);
    xdg_toplevel_icon_v1_destroy(early_icon);
    wlc_destroy(early);
    wlc_roundtrip(c);

    wlc_phase("setting an icon name");
    struct xdg_toplevel_icon_v1* named = xdg_toplevel_icon_manager_v1_create_icon(fi_manager);
    xdg_toplevel_icon_v1_set_name(named, "text-editor");
    xdg_toplevel_icon_manager_v1_set_icon(fi_manager, w->toplevel, named);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    fi_icon_of("fenriz-test icon-window", icon, sizeof icon);
    if (strcmp(icon, "text-editor"))
        wlc_die("icon name did not reach the feed (got \"%s\")", icon);

    // An icon carrying only pixels has no name to publish. Reporting the previous name here
    // would be worse than reporting none — the bar would draw the wrong app.
    wlc_phase("replacing it with a buffer-only icon");
    struct xdg_toplevel_icon_v1* pixels = xdg_toplevel_icon_manager_v1_create_icon(fi_manager);
    struct wl_buffer* buf = wlc_buffer(c, 64, 64, GREEN);
    xdg_toplevel_icon_v1_add_buffer(pixels, buf, 1);
    xdg_toplevel_icon_manager_v1_set_icon(fi_manager, w->toplevel, pixels);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    fi_icon_of("fenriz-test icon-window", icon, sizeof icon);
    if (icon[0])
        wlc_die("buffer-only icon left \"%s\" published; a stale name is a wrong icon", icon);

    wlc_phase("unsetting the icon");
    xdg_toplevel_icon_manager_v1_set_icon(fi_manager, w->toplevel, NULL);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    fi_icon_of("fenriz-test icon-window", icon, sizeof icon);
    if (icon[0])
        wlc_die("icon survived being unset: \"%s\"", icon);
    wlc_log("name published, buffer-only and unset both cleared");

    wlc_hold_point(c);

    // The icon objects outlive the toplevel here on purpose: destroying them after the
    // window is the ordering a client shutting down produces.
    wlc_destroy(w);
    wlc_roundtrip(c);
    xdg_toplevel_icon_v1_destroy(named);
    xdg_toplevel_icon_v1_destroy(pixels);
    wl_buffer_destroy(buf);
    wlc_roundtrip(c);
}

// --- toplevel-drag --------------------------------------------------------------------

static struct xdg_toplevel_drag_manager_v1* td_manager;

static void td_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, xdg_toplevel_drag_manager_v1_interface.name))
        td_manager = wl_registry_bind(reg, name, &xdg_toplevel_drag_manager_v1_interface, 1);
}
static void td_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener td_registry_listener = {
    .global = td_registry_global,
    .global_remove = td_registry_remove,
};

// Drag focus, i.e. which of our surfaces the compositor considers the drop target. The drag
// is our own, so we receive our own dnd events and can see the target it picks.
static struct wl_surface* td_dnd_surface;

static void td_dd_offer(void* d, struct wl_data_device* dd, struct wl_data_offer* o) {
    (void)d;
    (void)dd;
    (void)o;
}
static struct wl_data_offer* td_offer;

static void td_dd_enter(void* d,
                        struct wl_data_device* dd,
                        uint32_t serial,
                        struct wl_surface* s,
                        wl_fixed_t x,
                        wl_fixed_t y,
                        struct wl_data_offer* o) {
    (void)d;
    (void)dd;
    (void)x;
    (void)y;
    td_dnd_surface = s;
    td_offer = o;
    // Accepting, and then never finishing, is what leaves the drop pending — so the seat drag
    // outlives the button release. That is the window in which a client destroys its
    // xdg_toplevel_drag object, and the compositor still has to put the window back.
    if (o) {
        wl_data_offer_accept(o, serial, "text/plain");
        if (wl_data_offer_get_version(o) >= 3)
            wl_data_offer_set_actions(
                o, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    }
}
static void td_dd_leave(void* d, struct wl_data_device* dd) {
    (void)d;
    (void)dd;
    td_dnd_surface = NULL;
}
static void td_dd_motion(void* d, struct wl_data_device* dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)dd;
    (void)t;
    (void)x;
    (void)y;
}
static void td_dd_drop(void* d, struct wl_data_device* dd) {
    (void)d;
    (void)dd;
}
static void td_dd_selection(void* d, struct wl_data_device* dd, struct wl_data_offer* o) {
    (void)d;
    (void)dd;
    (void)o;
}
static const struct wl_data_device_listener td_dd_listener = {
    .data_offer = td_dd_offer,
    .enter = td_dd_enter,
    .leave = td_dd_leave,
    .motion = td_dd_motion,
    .drop = td_dd_drop,
    .selection = td_dd_selection,
};

#define BTN_LEFT 0x110

// One tear-off: press on `parent`, start a drag, attach a fresh toplevel at offset 20,30,
// map it, and walk the cursor to the drop point. Leaves the button held.
struct td_run {
    struct win* tab;
    struct xdg_toplevel_drag_v1* drag;
    struct wl_data_source* src;
};

static struct td_run td_tear_off(struct wlc* c, struct win* parent, const char* title, int px, int py, int dx, int dy) {
    wlc_pointer_to(c, px, py);
    wlc_pump(c, 60);
    if (c->enter_surface != parent->surface)
        wlc_die("injected pointer is not over the window to tear from; no drag can start here");
    wlc_pointer_button(c, BTN_LEFT, true);
    wlc_pump(c, 60);

    struct td_run r = {0};
    r.src = wl_data_device_manager_create_data_source(c->ddm);
    wl_data_source_add_listener(r.src, &source_listener, NULL);
    wl_data_source_offer(r.src, "text/plain");
    if (wl_data_source_get_version(r.src) >= 3)
        wl_data_source_set_actions(r.src, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    r.drag = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(td_manager, r.src);
    wl_data_device_start_drag(c->data_device, r.src, parent->surface, NULL, c->last_serial);
    wlc_roundtrip(c);

    // The torn-off tab: attached before it maps, which is the order the protocol describes.
    r.tab = wlc_toplevel(c, 200, 150, title);
    xdg_toplevel_drag_v1_attach(r.drag, r.tab->toplevel, 20, 30);
    wlc_map(r.tab, GREEN);
    wlc_pointer_to(c, dx, dy);
    wlc_pump(c, 100);
    return r;
}

static void td_drop(struct wlc* c, struct td_run* r) {
    wlc_pointer_button(c, BTN_LEFT, false);
    // Destroyed immediately, which is what the protocol tells a client to do once the drop is
    // performed — and that can land while the seat drag is still alive. Waiting first would let
    // the seat drag die first and quietly test the easy ordering instead.
    xdg_toplevel_drag_v1_destroy(r->drag);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    wl_data_source_destroy(r->src);
    wlc_roundtrip(c);
}

static void s_toplevel_drag(struct wlc* c) {
    td_manager = NULL;
    td_dnd_surface = NULL;

    wlc_phase("binding xdg_toplevel_drag_manager_v1");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &td_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!td_manager)
        wlc_die("compositor does not advertise xdg_toplevel_drag_manager_v1");
    if (!c->data_device)
        wlc_die("no wl_data_device_manager; a toplevel drag rides on a normal drag");
    wl_data_device_add_listener(c->data_device, &td_dd_listener, NULL);

    // --- torn off a tiled window: the tab joins the layout on drop ---
    wlc_phase("tearing a tab off a tiled window");
    struct win* parent = wlc_toplevel(c, 400, 300, "fenriz-test drag-parent");
    wlc_map(parent, BLUE);
    struct td_run t1 = td_tear_off(c, parent, "fenriz-test drag-tab", 200, 200, 400, 300);

    if (!fe_is_floating("fenriz-test drag-tab"))
        wlc_die("the attached window tiled mid-drag; it cannot follow the cursor from the layout");
    // Control: same client, same app_id, not attached — proves the float comes from the drag.
    if (fe_is_floating("fenriz-test drag-parent"))
        wlc_die("the parent floated too; the float is not coming from the toplevel drag");
    // The dragged window sits under the cursor, so it would swallow every drop target if the
    // compositor let it. Reattaching a tab depends on the window behind it staying reachable.
    if (td_dnd_surface == t1.tab->surface)
        wlc_die("the dragged window became the drop target; it must not take part in that");
    if (td_dnd_surface != parent->surface)
        wlc_die("drop target is neither window; the drag lost its focus entirely");
    wlc_hold_point(c);

    wlc_phase("dropping the tab torn off a tiled window");
    td_drop(c, &t1);
    if (fe_is_floating("fenriz-test drag-tab"))
        wlc_die("a tab torn off a tiled window stayed floating; it should land the way its parent lives");
    wlc_log("tab torn off a tiled window joined the layout");

    wlc_destroy(t1.tab);
    wlc_destroy(parent);
    wlc_roundtrip(c);

    // --- torn off a floating window: the tab stays floating, where it was dropped ---
    if (c->out_w <= 0 || c->out_h <= 0)
        wlc_die("no output mode; cannot aim the pointer at a window the compositor centered");
    wlc_phase("tearing a tab off a floating window");
    struct win* fparent = wlc_toplevel(c, 400, 300, "fenriz-test drag-float-parent");
    // Pinned to one size: fenriz auto-floats such a window and centers it, so the middle of
    // the output is inside it without this test having to know where it was put.
    xdg_toplevel_set_min_size(fparent->toplevel, 400, 300);
    xdg_toplevel_set_max_size(fparent->toplevel, 400, 300);
    wlc_map(fparent, GREY);
    if (!fe_is_floating("fenriz-test drag-float-parent"))
        wlc_die("the fixed-size parent did not float; this half of the test proves nothing");

    struct td_run t2 = td_tear_off(c, fparent, "fenriz-test drag-float-tab", c->out_w / 2, c->out_h / 2, 200, 200);
    wlc_hold_point(c);
    wlc_phase("dropping the tab torn off a floating window");
    td_drop(c, &t2);
    if (!fe_is_floating("fenriz-test drag-float-tab"))
        wlc_die("a tab torn off a floating window tiled; it should stay floating like its parent");

    // The window is never told where it is, so ask the compositor by walking the pointer one
    // pixel: the enter coordinates say exactly where the window ended up. It was dropped with
    // the cursor at 200,200 holding it at offset 20,30, so 201,201 lands 21,31 into it.
    c->enter_surface = NULL;
    wlc_pointer_to(c, 201, 201);
    wlc_pump(c, 100);
    if (c->enter_surface != t2.tab->surface)
        wlc_die("the pointer is not over the dragged window after the drop; it never followed");
    if (c->enter_sx != 21 || c->enter_sy != 31)
        wlc_die("dragged window is %d,%d from the cursor; expected the 20,30 offset it asked for",
                c->enter_sx,
                c->enter_sy);
    wlc_log("window tracked the cursor to its requested offset and stayed there");

    wlc_destroy(t2.tab);
    wlc_destroy(fparent);
    wlc_roundtrip(c);
}

// --- toplevel-tag ---------------------------------------------------------------------

static struct xdg_toplevel_tag_manager_v1* tg_manager;

static void tg_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, xdg_toplevel_tag_manager_v1_interface.name))
        tg_manager = wl_registry_bind(reg, name, &xdg_toplevel_tag_manager_v1_interface, 1);
    else if (!strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name))
        fi_manager = wl_registry_bind(reg, name, &xdg_toplevel_icon_manager_v1_interface, 1);
}
static void tg_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener tg_registry_listener = {
    .global = tg_registry_global,
    .global_remove = tg_registry_remove,
};

// The `tag` the compositor publishes for the window with this title.
static void tg_tag_of(const char* title, char* out, size_t n) {
    char snap[16384];
    out[0] = 0;
    if (!ipc_snapshot(snap, sizeof snap))
        wlc_die("no control socket; cannot read the published tag");
    const char* p = strstr(snap, title);
    if (!p)
        wlc_die("window \"%s\" is not in the state snapshot", title);
    const char* t = strstr(p, "\"tag\":\"");
    if (!t)
        wlc_die("no tag field in the snapshot; the feed never grew one");
    t += 7;
    const char* end = strchr(t, '"');
    if (!end || (size_t)(end - t) >= n)
        return;
    snprintf(out, n, "%.*s", (int)(end - t), t);
}

static void s_toplevel_tag(struct wlc* c) {
    tg_manager = NULL;

    wlc_phase("binding xdg_toplevel_tag_manager_v1");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &tg_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!tg_manager)
        wlc_die("compositor does not advertise xdg_toplevel_tag_manager_v1");

    char tag[256];

    // The case the protocol actually asks for: the tag is set as part of the initial commit,
    // before the window maps. A compositor that only looks at mapped windows drops it here.
    wlc_phase("tagging a window before it maps");
    struct win* w = wlc_toplevel(c, 400, 300, "fenriz-test tag-window");
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(tg_manager, w->toplevel, "settings");
    wlc_map(w, BLUE);
    tg_tag_of("fenriz-test tag-window", tag, sizeof tag);
    if (strcmp(tag, "settings"))
        wlc_die("tag set before map did not reach the feed (got \"%s\")", tag);
    // tests/config/toplevel-tag.conf floats anything tagged `settings`. This is the only check
    // that the tag reaches the window rules at all — and, since that rule lives nowhere but the
    // test config, that this instance is reading it instead of the user's.
    if (!fe_is_floating("fenriz-test tag-window"))
        wlc_die("the `tag=^settings$` window rule did not apply; the tag never reached the rules");

    // Control: same client, same app_id, never tagged — proves the tag above is this window's
    // own and not something the feed prints for everything.
    struct win* plain = wlc_toplevel(c, 400, 300, "fenriz-test tag-untagged");
    wlc_map(plain, GREEN);
    tg_tag_of("fenriz-test tag-untagged", tag, sizeof tag);
    if (tag[0])
        wlc_die("a window that set no tag reports \"%s\"", tag);
    // Control for the rule above: same client, same app_id, no tag — so it tiles.
    if (fe_is_floating("fenriz-test tag-untagged"))
        wlc_die("an untagged window floated too; the rule is not keyed on the tag");

    // The protocol allows retagging at any time, "for example if the purpose of the toplevel
    // changes". Window rules already ran, but the feed has to follow.
    wlc_phase("retagging a mapped window");
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(tg_manager, w->toplevel, "composer");
    wlc_roundtrip(c);
    wlc_pump(c, 100);
    tg_tag_of("fenriz-test tag-window", tag, sizeof tag);
    if (strcmp(tag, "composer"))
        wlc_die("retagging a mapped window did not reach the feed (got \"%s\")", tag);

    // Tag and icon are two protocols kept in one per-window record, so setting one must not
    // disturb the other.
    wlc_phase("giving the same window an icon as well as a tag");
    if (!fi_manager)
        wlc_die("compositor does not advertise xdg_toplevel_icon_manager_v1");
    struct xdg_toplevel_icon_v1* ic = xdg_toplevel_icon_manager_v1_create_icon(fi_manager);
    xdg_toplevel_icon_v1_set_name(ic, "mail-client");
    xdg_toplevel_icon_manager_v1_set_icon(fi_manager, w->toplevel, ic);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    char icon[256];
    fi_icon_of("fenriz-test tag-window", icon, sizeof icon);
    if (strcmp(icon, "mail-client"))
        wlc_die("icon on a tagged window did not reach the feed (got \"%s\")", icon);
    tg_tag_of("fenriz-test tag-window", tag, sizeof tag);
    if (strcmp(tag, "composer"))
        wlc_die("setting an icon clobbered the tag (now \"%s\")", tag);
    // ...and the other way round: retag now that an icon is set, and the icon must survive it.
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(tg_manager, w->toplevel, "inbox");
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    fi_icon_of("fenriz-test tag-window", icon, sizeof icon);
    if (strcmp(icon, "mail-client"))
        wlc_die("setting a tag clobbered the icon (now \"%s\")", icon);
    tg_tag_of("fenriz-test tag-window", tag, sizeof tag);
    if (strcmp(tag, "inbox"))
        wlc_die("retagging a window that has an icon did not take (got \"%s\")", tag);
    xdg_toplevel_icon_v1_destroy(ic);
    wlc_roundtrip(c);

    // A description is legal and fenriz ignores it; it must not upset anything.
    xdg_toplevel_tag_manager_v1_set_toplevel_description(tg_manager, w->toplevel, "E-mail composer");
    wlc_roundtrip(c);
    wlc_hold_point(c);

    // The tag outlives nothing: closing the window has to take its entry with it.
    wlc_phase("destroying a tagged window");
    wlc_destroy(w);
    wlc_roundtrip(c);
    wlc_pump(c, 100);

    // A fresh window that sets no tag must not inherit the dead one's entry.
    struct win* after = wlc_toplevel(c, 300, 200, "fenriz-test tag-after");
    wlc_map(after, RED);
    tg_tag_of("fenriz-test tag-after", tag, sizeof tag);
    if (tag[0])
        wlc_die("a new untagged window picked up \"%s\" from the destroyed one", tag);

    wlc_destroy(after);
    wlc_destroy(plain);
    xdg_toplevel_tag_manager_v1_destroy(tg_manager);
    wlc_roundtrip(c);
}

// --- bell / alpha / fixes -------------------------------------------------------------

static struct xdg_system_bell_v1* sm_bell;
static struct wp_alpha_modifier_v1* sm_alpha;
static struct wl_fixes* sm_fixes;

static void sm_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, xdg_system_bell_v1_interface.name))
        sm_bell = wl_registry_bind(reg, name, &xdg_system_bell_v1_interface, 1);
    else if (!strcmp(iface, wp_alpha_modifier_v1_interface.name))
        sm_alpha = wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
    else if (!strcmp(iface, wl_fixes_interface.name))
        sm_fixes = wl_registry_bind(reg, name, &wl_fixes_interface, 1);
}
static void sm_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener sm_registry_listener = {
    .global = sm_registry_global,
    .global_remove = sm_registry_remove,
};

static void sm_bind(struct wlc* c) {
    sm_bell = NULL;
    sm_alpha = NULL;
    sm_fixes = NULL;
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &sm_registry_listener, NULL);
    wlc_roundtrip(c);
}

// Whether the compositor is flagging this window as wanting attention.
static bool sm_is_urgent(const char* title) {
    char snap[16384];
    if (!ipc_snapshot(snap, sizeof snap))
        wlc_die("no control socket; cannot read the urgent flag");
    const char* p = strstr(snap, title);
    if (!p)
        wlc_die("window \"%s\" is not in the state snapshot", title);
    const char* u = strstr(p, "\"urgent\":");
    if (!u)
        wlc_die("no urgent field in the snapshot; the feed never grew one");
    return !strncmp(u + 9, "true", 4);
}

// Ring, let the compositor settle, and return what the event feed reported. `ev` may be -1
// when there is no event socket, in which case the checks are skipped rather than failed.
static void sm_ring(struct wlc* c, int ev, struct wl_surface* surface, char* out, size_t n) {
    out[0] = 0;
    xdg_system_bell_v1_ring(sm_bell, surface);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    if (ev >= 0)
        ipc_event_drain(ev, out, n, 200);
}

static void s_bell(struct wlc* c) {
    sm_bind(c);
    if (!sm_bell)
        wlc_die("compositor does not advertise xdg_system_bell_v1");

    // Two tiled windows: both on screen, the second one focused because it mapped last.
    struct win* bg = wlc_toplevel(c, 400, 300, "fenriz-test bell-window");
    wlc_map(bg, BLUE);
    struct win* fg = wlc_toplevel(c, 400, 300, "fenriz-test bell-focused");
    wlc_map(fg, GREEN);
    wlc_pump(c, 100);

    if (sm_is_urgent("fenriz-test bell-window"))
        wlc_die("a window nobody rang is already urgent");

    // The event feed carries no backlog, so subscribe before ringing anything.
    int ev = ipc_event_connect();
    if (ev < 0)
        wlc_log("no event socket; checking the urgent flag only");
    char feed[8192];

    // The bell's whole point is the window you are NOT typing into: it is on screen, so an
    // xdg-activation request would be ignored, but a bell there is exactly what to flag.
    wlc_phase("ringing the bell on the unfocused window");
    sm_ring(c, ev, bg->surface, feed, sizeof feed);
    if (!sm_is_urgent("fenriz-test bell-window"))
        wlc_die("ringing the bell on an unfocused window did not flag it");
    // Control: the bell is attributed to one window, not sprayed across the client.
    if (sm_is_urgent("fenriz-test bell-focused"))
        wlc_die("the bell flagged the focused window too; it is not attributed to a surface");
    if (ev >= 0) {
        if (count_substr(feed, "\"event\":\"bell\"") != 1)
            wlc_die("expected exactly one bell event, got: %s", feed);
        if (!strstr(feed, "fenriz-test bell-window"))
            wlc_die("the bell event does not name the window that rang: %s", feed);
    }

    // A bell in the window you are already looking at has nothing to demand of the urgent
    // flag, but it is still a bell: the event has to fire where the flag cannot.
    wlc_phase("ringing the bell on the focused window");
    sm_ring(c, ev, fg->surface, feed, sizeof feed);
    if (sm_is_urgent("fenriz-test bell-focused"))
        wlc_die("the focused window was flagged urgent; focus is what clears that flag");
    if (ev >= 0 && count_substr(feed, "\"event\":\"bell\"") != 1)
        wlc_die("a bell on the focused window published no event; urgent is not the only channel: %s", feed);

    // Two rings with nothing else changing. The state feed suppresses identical snapshots, so
    // this is exactly the case an event channel exists to carry.
    wlc_phase("ringing the same window twice");
    xdg_system_bell_v1_ring(sm_bell, bg->surface);
    sm_ring(c, ev, bg->surface, feed, sizeof feed);
    if (ev >= 0 && count_substr(feed, "\"event\":\"bell\"") != 2)
        wlc_die("two rings did not produce two events; they were deduplicated: %s", feed);

    // A surfaceless bell is legal ("not tied to a particular window") and must not upset it.
    wlc_phase("ringing a surfaceless bell");
    sm_ring(c, ev, NULL, feed, sizeof feed);
    if (ev >= 0) {
        if (count_substr(feed, "\"event\":\"bell\"") != 1)
            wlc_die("a surfaceless bell published no event: %s", feed);
        if (strstr(feed, "\"appId\""))
            wlc_die("a bell tied to no surface named a window anyway: %s", feed);
    }
    wlc_hold_point(c);

    // The event feed is a separate channel: the state socket must carry state and nothing else.
    char snap[16384];
    if (ev >= 0 && ipc_snapshot(snap, sizeof snap) && strstr(snap, "\"event\""))
        wlc_die("an event leaked into the state feed: %s", snap);

    if (ev >= 0)
        close(ev);
    xdg_system_bell_v1_destroy(sm_bell);
    wlc_destroy(fg);
    wlc_destroy(bg);
    wlc_roundtrip(c);
}

static void s_alpha(struct wlc* c) {
    sm_bind(c);
    if (!sm_alpha)
        wlc_die("compositor does not advertise wp_alpha_modifier_v1");

    struct win* w = wlc_toplevel(c, 400, 300, "fenriz-test alpha-window");
    wlc_map(w, BLUE);

    // Note this asserts the protocol is accepted and survives, NOT that the window is drawn
    // any dimmer: opacity is not in the state feed, and the headless backend renders into
    // memory nothing here can read back. The compositor-side multiply lives in apply_fx.
    wlc_phase("setting a surface multiplier");
    struct wp_alpha_modifier_surface_v1* mod = wp_alpha_modifier_v1_get_surface(sm_alpha, w->surface);
    wp_alpha_modifier_surface_v1_set_multiplier(mod, UINT32_MAX / 2); // ~0.5
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    wlc_phase("walking the multiplier over its whole range");
    wp_alpha_modifier_surface_v1_set_multiplier(mod, 0); // fully transparent, still mapped
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 100);
    wp_alpha_modifier_surface_v1_set_multiplier(mod, UINT32_MAX); // fully opaque again
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 100);
    wlc_hold_point(c);

    // Destroying the modifier drops back to the compositor's own opacity; the surface must
    // outlive it cleanly.
    wp_alpha_modifier_surface_v1_destroy(mod);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 100);
    // sm_is_urgent dies if the window is gone from the feed, so this is the "still mapped and
    // healthy after the modifier was destroyed" check.
    if (sm_is_urgent("fenriz-test alpha-window"))
        wlc_die("the window came back urgent from an opacity change");

    wp_alpha_modifier_v1_destroy(sm_alpha);
    wlc_destroy(w);
    wlc_roundtrip(c);
}

static void s_fixes(struct wlc* c) {
    sm_bind(c);
    if (!sm_fixes)
        wlc_die("compositor does not advertise wl_fixes");

    // The entire point of the protocol: a registry can be destroyed. Without it libwayland
    // leaks one per bind for the life of the connection.
    wlc_phase("destroying registries through wl_fixes");
    for (int i = 0; i < 8; i++) {
        struct wl_registry* reg = wl_display_get_registry(c->display);
        wlc_roundtrip(c); // let it fill with globals before throwing it away
        wl_fixes_destroy_registry(sm_fixes, reg);
        wlc_roundtrip(c);
    }

    // Still serving afterwards: bind a fresh registry and map a window through it.
    struct win* w = wlc_toplevel(c, 300, 200, "fenriz-test fixes-window");
    wlc_map(w, GREEN);
    wlc_pump(c, 100);
    if (sm_is_urgent("fenriz-test fixes-window"))
        wlc_die("unexpected state for a freshly mapped window");
    wlc_hold_point(c);

    wl_fixes_destroy(sm_fixes);
    wlc_destroy(w);
    wlc_roundtrip(c);
}

// --- background-effect ----------------------------------------------------------------

static struct ext_background_effect_manager_v1* be_manager;
static uint32_t be_caps;
static bool be_got_caps;

static void be_capabilities(void* d, struct ext_background_effect_manager_v1* m, uint32_t flags) {
    (void)d;
    (void)m;
    be_caps = flags;
    be_got_caps = true;
    wlc_log("background effect capabilities 0x%x", flags);
}
static const struct ext_background_effect_manager_v1_listener be_manager_listener = {
    .capabilities = be_capabilities,
};

static void be_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, ext_background_effect_manager_v1_interface.name)) {
        be_manager = wl_registry_bind(reg, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(be_manager, &be_manager_listener, NULL);
    }
}
static void be_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener be_registry_listener = {
    .global = be_registry_global,
    .global_remove = be_registry_remove,
};

// Two effect objects on one surface is a background_effect_exists error; the client that does
// it is expected to be killed, and the compositor to keep serving everyone else.
static void be_double_effect(struct wlc* c) {
    struct wl_registry* reg = wl_display_get_registry(c->display);
    be_manager = NULL;
    wl_registry_add_listener(reg, &be_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!be_manager)
        return;
    struct win* w = wlc_toplevel(c, 300, 200, "fenriz-test be-abuse");
    wlc_map(w, BLUE);
    ext_background_effect_manager_v1_get_background_effect(be_manager, w->surface);
    ext_background_effect_manager_v1_get_background_effect(be_manager, w->surface);
    wlc_roundtrip(c);
}

static void s_background_effect(struct wlc* c) {
    be_manager = NULL;
    be_caps = 0;
    be_got_caps = false;

    wlc_phase("binding ext_background_effect_manager_v1");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &be_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!be_manager)
        wlc_die("compositor does not advertise ext_background_effect_manager_v1");
    wlc_roundtrip(c); // the bind only reaches the server at the end of the roundtrip above

    // Capabilities arrive on bind. tests/config/background-effect.conf turns blur on, so the
    // bit has to be set — which also proves the config knob reaches the protocol.
    if (!be_got_caps)
        wlc_die("no capabilities event on bind; clients cannot tell what is supported");
    if (!(be_caps & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR))
        wlc_die("compositor reports no blur capability (0x%x) with blur enabled in its config", be_caps);

    struct win* w = wlc_toplevel(c, 400, 300, "fenriz-test be-window");
    wlc_map(w, BLUE);

    wlc_phase("setting a blur region");
    struct ext_background_effect_surface_v1* fx =
        ext_background_effect_manager_v1_get_background_effect(be_manager, w->surface);
    struct wl_region* r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 200, 150);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r); // copy semantics: legal immediately
    wlc_paint(w, BLUE);   // the region is double-buffered; this commit is what applies it
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    // A region of several rectangles, which is one blur node each on the compositor side.
    wlc_phase("replacing it with a multi-rectangle region");
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 400, 40);
    wl_region_add(r, 0, 260, 400, 40);
    wl_region_add(r, 180, 40, 40, 220);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    // Setting a region without committing must not apply it, and must not upset anything.
    wlc_phase("leaving a region pending, uncommitted");
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 50, 50);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_roundtrip(c);
    wlc_pump(c, 100);

    wlc_phase("clearing the region with NULL");
    ext_background_effect_surface_v1_set_blur_region(fx, NULL);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 150);
    wlc_hold_point(c);

    // Destroying the effect with a region still live is the ordinary teardown path.
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 100, 100);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    ext_background_effect_surface_v1_destroy(fx);
    wlc_roundtrip(c);
    wlc_pump(c, 100);

    // A second effect object is legal once the first is gone.
    wlc_phase("re-attaching an effect after destroying the first");
    struct ext_background_effect_surface_v1* fx2 =
        ext_background_effect_manager_v1_get_background_effect(be_manager, w->surface);
    wlc_roundtrip(c);
    ext_background_effect_surface_v1_destroy(fx2);

    // An effect whose surface dies under it goes inert rather than taking anything with it.
    wlc_phase("destroying a window under a live effect");
    struct win* doomed = wlc_toplevel(c, 300, 200, "fenriz-test be-doomed");
    wlc_map(doomed, GREEN);
    struct ext_background_effect_surface_v1* fx3 =
        ext_background_effect_manager_v1_get_background_effect(be_manager, doomed->surface);
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 300, 200);
    ext_background_effect_surface_v1_set_blur_region(fx3, r);
    wl_region_destroy(r);
    wlc_paint(doomed, GREEN);
    wlc_roundtrip(c);
    wlc_destroy(doomed);
    wlc_roundtrip(c);
    ext_background_effect_surface_v1_destroy(fx3); // the object outlives its surface
    wlc_roundtrip(c);

    // A popup is in neither the view list nor the layer list, so it reaches its blur nodes
    // by a path of its own. This is what a GTK menu on the desktop shell does.
    wlc_phase("blurring a popup");
    struct win* menu = wlc_popup(w, 40, 40, 220, 160, false);
    struct ext_background_effect_surface_v1* fx4 =
        ext_background_effect_manager_v1_get_background_effect(be_manager, menu->surface);
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 4000, 4000); // clipped to the popup by the compositor
    ext_background_effect_surface_v1_set_blur_region(fx4, r);
    wl_region_destroy(r);
    wlc_map(menu, 0x40202020u);
    wlc_pump(c, 200);
    wlc_hold_point(c);

    // Nested submenus land in the same parent tree, so a second blurred popup must not
    // disturb the first.
    wlc_phase("blurring a nested popup");
    struct win* submenu = wlc_popup(menu, 30, 30, 160, 110, false);
    struct ext_background_effect_surface_v1* fx5 =
        ext_background_effect_manager_v1_get_background_effect(be_manager, submenu->surface);
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 4000, 4000);
    ext_background_effect_surface_v1_set_blur_region(fx5, r);
    wl_region_destroy(r);
    wlc_map(submenu, 0x40202020u);
    wlc_pump(c, 200);

    // Teardown is inner-first, the order a menu closes in. The blur nodes live inside each
    // popup's own tree, so they have to go with it and leave nothing behind.
    wlc_phase("tearing the blurred popups down");
    ext_background_effect_surface_v1_destroy(fx5);
    wlc_destroy(submenu);
    wlc_roundtrip(c);
    ext_background_effect_surface_v1_destroy(fx4);
    wlc_destroy(menu);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    if (!wlc_abuse(c, "two background effects on one surface", be_double_effect))
        wlc_die("a second effect on one surface was accepted; background_effect_exists is not enforced");

    ext_background_effect_manager_v1_destroy(be_manager);
    wlc_destroy(w);
    wlc_roundtrip(c);
}

// --- kde-blur -------------------------------------------------------------------------

static struct org_kde_kwin_blur_manager* kb_manager;

static void kb_registry_global(void* d, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    (void)ver;
    if (!strcmp(iface, org_kde_kwin_blur_manager_interface.name))
        kb_manager = wl_registry_bind(reg, name, &org_kde_kwin_blur_manager_interface, 1);
}
static void kb_registry_remove(void* d, struct wl_registry* reg, uint32_t name) {
    (void)d;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener kb_registry_listener = {
    .global = kb_registry_global,
    .global_remove = kb_registry_remove,
};

static void s_kde_blur(struct wlc* c) {
    kb_manager = NULL;

    wlc_phase("binding org_kde_kwin_blur_manager");
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &kb_registry_listener, NULL);
    wlc_roundtrip(c);
    if (!kb_manager)
        wlc_die("compositor does not advertise org_kde_kwin_blur_manager");

    struct win* w = wlc_toplevel(c, 400, 300, "fenriz-test kde-blur-window");
    wlc_map(w, BLUE);

    // Exactly what Ghostty sends: create, commit, never a region. In this protocol that means
    // "blur the whole surface" — unlike ext-background-effect-v1, whose region starts empty and
    // blurs nothing. A compositor that treats them the same accepts all of this and draws no
    // blur at all, which is a silent no-op rather than an error.
    wlc_phase("create + commit with no region, the way Ghostty asks");
    struct org_kde_kwin_blur* b = org_kde_kwin_blur_manager_create(kb_manager, w->surface);
    org_kde_kwin_blur_commit(b);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    wlc_phase("narrowing it to a region, then back to the whole surface");
    struct wl_region* r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 10, 10, 100, 80);
    org_kde_kwin_blur_set_region(b, r);
    wl_region_destroy(r);
    org_kde_kwin_blur_commit(b);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    org_kde_kwin_blur_set_region(b, NULL); // NULL here means all of it, not none
    org_kde_kwin_blur_commit(b);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    // Creating a second blur for the same surface replaces the first; clients were written
    // against a compositor that allows it, so it must not be an error.
    wlc_phase("creating a second blur for the same surface");
    struct org_kde_kwin_blur* b2 = org_kde_kwin_blur_manager_create(kb_manager, w->surface);
    org_kde_kwin_blur_commit(b2);
    wlc_roundtrip(c);
    wlc_pump(c, 100);
    wlc_hold_point(c);

    wlc_phase("unset, then release");
    org_kde_kwin_blur_manager_unset(kb_manager, w->surface);
    wlc_paint(w, BLUE);
    wlc_roundtrip(c);
    org_kde_kwin_blur_release(b2);
    wlc_roundtrip(c);

    // A blur object outliving its surface must go inert, not take the compositor with it.
    wlc_phase("destroying a window under a live blur object");
    struct win* doomed = wlc_toplevel(c, 300, 200, "fenriz-test kde-blur-doomed");
    wlc_map(doomed, GREEN);
    struct org_kde_kwin_blur* b3 = org_kde_kwin_blur_manager_create(kb_manager, doomed->surface);
    org_kde_kwin_blur_commit(b3);
    wlc_paint(doomed, GREEN);
    wlc_roundtrip(c);
    wlc_destroy(doomed);
    wlc_roundtrip(c);
    org_kde_kwin_blur_set_region(b3, NULL); // legal, and must be ignored rather than crash
    org_kde_kwin_blur_release(b3);
    wlc_roundtrip(c);

    wlc_destroy(w);
    wlc_roundtrip(c);
}

// --- pixels ---------------------------------------------------------------------------
// Everything else in this file asserts that a protocol was accepted. This one asserts what
// the compositor actually drew, by taking a screenshot and reading it back — the only way to
// catch geometry and effects going wrong. The blur corner bug that shipped once (a blur
// rectangle painting over a window's rounded corner) is invisible to every other scenario
// here and is caught below.

static void s_pixels(struct wlc* c) {
    if (c->out_w <= 0 || c->out_h <= 0)
        wlc_die("no output mode; nothing to compute expected coordinates from");
    const int W = c->out_w, H = c->out_h;

    wlc_phase("mapping a window that fills the output");
    struct win* bg = wlc_toplevel(c, W, H, "fenriz-test pixels-bg");
    wlc_map(bg, RED);
    wlc_pump(c, 200);

    struct wlc_shot* shot = wlc_capture(c, 0);
    if (shot->width != W || shot->height != H)
        wlc_die("screenshot is %dx%d, the output is %dx%d", shot->width, shot->height, W, H);

    // The oracle's own self-test: with no gaps and no border the window IS the output, so its
    // colour has to be at the centre. If this fails the screenshot is not what we think it is
    // and nothing below means anything.
    const uint32_t centre = wlc_pixel(shot, W / 2, H / 2);
    if (!wlc_color_near(centre, RED, 4))
        wlc_die("centre of a full-screen window is %s, expected %s", wlc_color_str(centre), wlc_color_str(RED));

    // Rounded corners cut the window away, so the very corner shows the desktop behind it.
    // A window drawn square would report its own colour here.
    const uint32_t corner = wlc_pixel(shot, 1, 1);
    if (wlc_color_near(corner, RED, 24))
        wlc_die("pixel 1,1 is %s — the window's own colour, so its corner was not rounded", wlc_color_str(corner));
    // ...and just inside the curve it is the window again, which is what stops the check above
    // passing for a window that simply is not there.
    const uint32_t inside = wlc_pixel(shot, 40, 40);
    if (!wlc_color_near(inside, RED, 4))
        wlc_die("pixel 40,40 is %s, expected the window colour %s just inside the corner radius",
                wlc_color_str(inside),
                wlc_color_str(RED));
    wlc_shot_free(shot);
    wlc_log("full-screen window, centre and rounded corner all where they should be");

    // A second window, floated and centred by the scenario's config so it sits over the middle
    // of the red one, translucent, and asking for blur across itself. Its rounded corner has to
    // show the red window crisply: the blur node is a rectangle, and if it is not rounded to
    // match, it paints its own processed version of red over exactly these pixels.
    wlc_phase("floating a blurred window over it");
    be_manager = NULL;
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &be_registry_listener, NULL);
    wlc_roundtrip(c);
    wlc_roundtrip(c);
    if (!be_manager)
        wlc_die("compositor does not advertise ext_background_effect_manager_v1");

    struct win* top = wlc_toplevel(c, 300, 220, "fenriz-test pixels-float");
    struct ext_background_effect_surface_v1* fx =
        ext_background_effect_manager_v1_get_background_effect(be_manager, top->surface);
    struct wl_region* r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 300, 220);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_map(top, 0x30202020u); // premultiplied, mostly transparent: the blur is what shows
    wlc_pump(c, 300);

    // Centred with no gaps and no border, so its top-left is exactly here.
    const int fw = 300, fh = 220;
    const int fx0 = (W - fw) / 2, fy0 = (H - fh) / 2;

    shot = wlc_capture(c, 0);
    // Inside the float, the red behind is blurred and tinted, so it must NOT still read as
    // plain red — otherwise the blur never happened and the corner check below proves nothing.
    const uint32_t middle = wlc_pixel(shot, fx0 + fw / 2, fy0 + fh / 2);
    if (wlc_color_near(middle, RED, 4))
        wlc_die("centre of the blurred window is plain %s; no blur was drawn, so the corner "
                "assertion cannot fail either",
                wlc_color_str(middle));
    // The corner: rounded away, so the red window shows through untouched.
    const uint32_t fcorner = wlc_pixel(shot, fx0 + 2, fy0 + 2);
    if (!wlc_color_near(fcorner, RED, 8))
        wlc_die("pixel %d,%d is %s, expected the window behind (%s) — the blur squared off the "
                "rounded corner",
                fx0 + 2,
                fy0 + 2,
                wlc_color_str(fcorner),
                wlc_color_str(RED));
    wlc_shot_free(shot);
    wlc_log("blur is drawn, and stops at the rounded corner");

    // A region far bigger than the surface. The protocol says the compositor clips it to the
    // surface; a compositor that takes it at face value blurs a chunk of the screen the window
    // does not own — and any client could ask for that.
    wlc_phase("asking to blur far more than the window");
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, -500, -500, 4000, 4000);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_paint(top, 0x30202020u);
    wlc_roundtrip(c);
    wlc_pump(c, 250);

    shot = wlc_capture(c, 0);
    // Well outside the float, over the red window: still crisp red if the region was clipped.
    const uint32_t outside = wlc_pixel(shot, fx0 - 60, fy0 + fh / 2);
    if (!wlc_color_near(outside, RED, 8))
        wlc_die("pixel %d,%d is %s, expected %s — blur escaped the window that asked for it",
                fx0 - 60,
                fy0 + fh / 2,
                wlc_color_str(outside),
                wlc_color_str(RED));
    wlc_shot_free(shot);
    wlc_log("an oversized region stays inside the window");

    ext_background_effect_surface_v1_destroy(fx);
    wlc_destroy(top);
    wlc_roundtrip(c);

    // A popup asking for blur — a menu on a desktop shell. Popups are in neither the view
    // list nor the layer list, so they reach their blur nodes their own way, and getting the
    // offset wrong draws the blur somewhere the popup is not.
    // The blur node is masked by the client's own buffer, so blur is only drawn where the
    // client drew something. A surface that painted nothing must therefore leave the pixels
    // behind it untouched, even though it asked for blur across the whole of itself. Without
    // the mask this is a blurred rectangle, and every rounded card gets square blur corners.
    wlc_phase("blurring a surface that painted nothing");
    struct win* ghost = wlc_toplevel(c, 300, 220, "fenriz-test pixels-float");
    struct ext_background_effect_surface_v1* gfx =
        ext_background_effect_manager_v1_get_background_effect(be_manager, ghost->surface);
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 300, 220);
    ext_background_effect_surface_v1_set_blur_region(gfx, r);
    wl_region_destroy(r);
    wlc_map(ghost, 0x00000000u); // premultiplied: nothing at all
    wlc_pump(c, 300);

    shot = wlc_capture(c, 0);
    const uint32_t ghost_mid = wlc_pixel(shot, W / 2, H / 2);
    if (!wlc_color_near(ghost_mid, RED, 6))
        wlc_die("pixel %d,%d is %s, expected %s — blur was drawn under a surface that painted "
                "nothing, so the transparency mask is not being applied",
                W / 2,
                H / 2,
                wlc_color_str(ghost_mid),
                wlc_color_str(RED));
    wlc_shot_free(shot);
    wlc_log("blur follows what the client actually painted");

    ext_background_effect_surface_v1_destroy(gfx);
    wlc_destroy(ghost);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    // The popup's own translucent buffer already tints the red behind it, so a single frame
    // proves nothing. Shoot the same popup with and without a region and require the pixel
    // to move: only the blur can account for the difference.
    wlc_phase("blurring a popup over the window");
    const int px = 100, py = 100, pw = 200, ph = 150;
    struct win* menu = wlc_popup(bg, px, py, pw, ph, false);
    wlc_map(menu, 0x30202020u);
    wlc_pump(c, 300);

    shot = wlc_capture(c, 0);
    const uint32_t before = wlc_pixel(shot, px + pw / 2, py + ph / 2);
    const uint32_t beside = wlc_pixel(shot, px + pw + 40, py + ph / 2);
    wlc_shot_free(shot);

    struct ext_background_effect_surface_v1* pfx =
        ext_background_effect_manager_v1_get_background_effect(be_manager, menu->surface);
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, pw, ph);
    ext_background_effect_surface_v1_set_blur_region(pfx, r);
    wl_region_destroy(r);
    wlc_paint(menu, 0x30202020u);
    wlc_roundtrip(c);
    wlc_pump(c, 300);

    shot = wlc_capture(c, 0);
    const uint32_t after = wlc_pixel(shot, px + pw / 2, py + ph / 2);
    if (wlc_color_near(before, after, 4))
        wlc_die("the popup's centre is still %s after asking for blur; nothing was drawn for a popup",
                wlc_color_str(after));
    // Just past its right edge: unchanged, so the nodes are where the popup is and not off at
    // the parent's origin.
    const uint32_t beside_after = wlc_pixel(shot, px + pw + 40, py + ph / 2);
    if (!wlc_color_near(beside, beside_after, 4))
        wlc_die("pixel %d,%d went from %s to %s — the popup's blur is drawn outside it",
                px + pw + 40,
                py + ph / 2,
                wlc_color_str(beside),
                wlc_color_str(beside_after));
    wlc_shot_free(shot);
    wlc_log("a popup's blur is drawn, and only where the popup is");

    ext_background_effect_surface_v1_destroy(pfx);
    wlc_destroy(menu);
    wlc_roundtrip(c);
    wlc_destroy(bg);
    wlc_roundtrip(c);
}

// --- layer-blur -----------------------------------------------------------------------
// Blur on a layer surface (a bar), which is where the node lifetime is hardest: the blur nodes
// are siblings of the surface's scene subtree, so nothing moves, hides or frees them with it.

static void s_layer_blur(struct wlc* c) {
    be_manager = NULL;
    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &be_registry_listener, NULL);
    wlc_roundtrip(c);
    wlc_roundtrip(c);
    if (!be_manager)
        wlc_die("compositor does not advertise ext_background_effect_manager_v1");
    if (!c->layer_shell)
        wlc_die("compositor has no zwlr_layer_shell_v1");

    // Something behind the bar to blur.
    struct win* bg = wlc_toplevel(c, 400, 300, "fenriz-test layer-blur-bg");
    wlc_map(bg, RED);

    wlc_phase("mapping a blurred layer surface");
    struct win* bar = wlc_layer(c, "fenriz-test-bar", ZWLR_LAYER_SHELL_V1_LAYER_TOP);
    struct ext_background_effect_surface_v1* fx =
        ext_background_effect_manager_v1_get_background_effect(be_manager, bar->surface);
    struct wl_region* r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 4000, 4000); // clipped to the surface by the compositor
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_map(bar, 0x40202020u);
    wlc_pump(c, 200);

    // Moving between layers reparents the surface's subtree. The blur nodes live in the tree it
    // came from, and putting them back under a surface that is now somewhere else is not
    // something the scene graph allows.
    wlc_phase("moving the blurred layer surface to another layer");
    zwlr_layer_surface_v1_set_layer(bar->layer, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    wl_surface_commit(bar->surface);
    wlc_roundtrip(c);
    wlc_pump(c, 200);
    zwlr_layer_surface_v1_set_layer(bar->layer, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    wl_surface_commit(bar->surface);
    wlc_roundtrip(c);
    wlc_pump(c, 200);
    wlc_hold_point(c);

    wlc_phase("tearing the blurred layer surface down");
    wlc_destroy(bar);
    wlc_roundtrip(c);
    wlc_pump(c, 150);

    // Re-map one, to prove the teardown left nothing behind that a second surface trips over.
    struct win* bar2 = wlc_layer(c, "fenriz-test-bar2", ZWLR_LAYER_SHELL_V1_LAYER_TOP);
    struct ext_background_effect_surface_v1* fx2 =
        ext_background_effect_manager_v1_get_background_effect(be_manager, bar2->surface);
    r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 200, 60);
    ext_background_effect_surface_v1_set_blur_region(fx2, r);
    wl_region_destroy(r);
    wlc_map(bar2, 0x40202020u);
    wlc_pump(c, 200);

    ext_background_effect_surface_v1_destroy(fx2);
    ext_background_effect_surface_v1_destroy(fx);
    wlc_destroy(bar2);
    wlc_destroy(bg);
    wlc_roundtrip(c);
}

// --- blur-off -------------------------------------------------------------------------

static void s_blur_off(struct wlc* c) {
    be_manager = NULL;
    be_caps = 0;
    be_got_caps = false;

    struct wl_registry* reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &be_registry_listener, NULL);
    wlc_roundtrip(c);
    wlc_roundtrip(c);
    if (!be_manager)
        wlc_die("compositor does not advertise ext_background_effect_manager_v1 with blur off");
    if (!be_got_caps)
        wlc_die("no capabilities event on bind");
    if (be_caps & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR)
        wlc_die("compositor claims it can blur (0x%x) with blur disabled in its config", be_caps);

    // Asking anyway is legal and must be accepted quietly — the protocol's answer to a missing
    // capability is that no effect is applied, not that the client is at fault.
    wlc_phase("asking for blur anyway");
    struct win* bg = wlc_toplevel(c, 400, 300, "fenriz-test blur-off-bg");
    wlc_map(bg, RED);
    struct ext_background_effect_surface_v1* fx =
        ext_background_effect_manager_v1_get_background_effect(be_manager, bg->surface);
    struct wl_region* r = wl_compositor_create_region(c->compositor);
    wl_region_add(r, 0, 0, 400, 300);
    ext_background_effect_surface_v1_set_blur_region(fx, r);
    wl_region_destroy(r);
    wlc_paint(bg, RED);
    wlc_roundtrip(c);
    wlc_pump(c, 200);

    // ...and nothing is drawn: with no gaps or border the window is the output, so its own
    // colour has to come back untouched by any blur pass.
    struct wlc_shot* shot = wlc_capture(c, 0);
    const uint32_t px = wlc_pixel(shot, c->out_w / 2, c->out_h / 2);
    if (!wlc_color_near(px, RED, 4))
        wlc_die("pixel is %s, expected an untouched %s — something was blurred with blur off",
                wlc_color_str(px),
                wlc_color_str(RED));
    wlc_shot_free(shot);

    ext_background_effect_surface_v1_destroy(fx);
    ext_background_effect_manager_v1_destroy(be_manager);
    wlc_destroy(bg);
    wlc_roundtrip(c);
}

const struct scenario scenarios[] = {
    {"popup", s_popup, "toplevel -> popup -> nested popup, grab, reposition, off-screen anchor"},
    {"layer-popup", s_layer_popup, "corner-anchored popup and submenu on a full-output layer surface"},
    {"resize", s_resize, "resize storm at illegal-looking-but-legal sizes, interactive resize"},
    {"fullscreen", s_fullscreen, "fullscreen toggling, per-output, with popups, before first map"},
    {"dnd", s_dnd, "start_drag with an icon; source and icon destroyed mid-drag"},
    {"grab", s_grab, "nested popup grabs, grab stack unwinding, illegal grabs"},
    {"subsurface", s_subsurface, "3-deep sync/desync subsurfaces, restacking, orphaning"},
    {"scale", s_scale, "buffer scale churn, fractional scale via viewport, crop/stretch"},
    {"destroy-parent", s_destroy_parent, "destroy the parent under a live nested popup"},
    {"hotplug", s_hotplug, "output enable/disable and lid churn under mapped surfaces"},
    {"workspace", s_workspace, "ext-workspace-v1: list, activate from the client, follow a switch"},
    {"dialog", s_dialog, "xdg-dialog-v1: a modal dialog holds focus against its parent"},
    {"icon", s_icon, "xdg-toplevel-icon-v1: icon name reaches the feed; buffer-only and unset clear it"},
    {"foreign", s_foreign, "xdg-foreign: export a toplevel, import it, parent a second window to it"},
    {"toplevel-tag",
     s_toplevel_tag,
     "xdg-toplevel-tag-v1: a tag set before map reaches the feed and dies with the window"},
    {"toplevel-drag", s_toplevel_drag, "xdg-toplevel-drag-v1: a tab torn out follows the cursor to the drop"},
    {"bell", s_bell, "xdg-system-bell-v1: a bell flags the window it names, not the focused one"},
    {"alpha", s_alpha, "alpha-modifier-v1: per-surface opacity accepted across its whole range"},
    {"fixes", s_fixes, "wl_fixes: registries can be destroyed and the compositor keeps serving"},
    {"background-effect", s_background_effect, "ext-background-effect-v1: capabilities, region latching, teardown"},
    {"kde-blur", s_kde_blur, "org_kde_kwin_blur: whole-surface default, regions, replace, unset"},
    {"pixels", s_pixels, "screenshot oracle: window geometry, rounded corners, blur stops at them"},
    {"layer-blur", s_layer_blur, "blur on a layer surface: layer changes, teardown, re-map"},
    {"blur-off", s_blur_off, "blur disabled: no capability advertised, nothing drawn"},
    {"evil", s_evil, "stale acks, commits between configures, self-resize, destroy/recreate"},
    {NULL, NULL, NULL},
};
