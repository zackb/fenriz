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

#include "ext-workspace-v1-client-protocol.h"
#include "xdg-foreign-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"

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

// Talk to the compositor's own control socket, the same one fenrizctl uses. Lets a
// scenario drive output/lid churn without the runner having to coordinate.
static int ipc_connect(void) {
    char path[256];
    const char* sock = getenv("FENRIZ_SOCKET");
    if (sock)
        snprintf(path, sizeof path, "%s", sock);
    else {
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const char* disp = getenv("WAYLAND_DISPLAY");
        if (!xdg || !disp)
            return -1;
        snprintf(path, sizeof path, "%s/fenriz-%s.sock", xdg, disp);
    }
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

static void ipc_send(const char* line) {
    int fd = ipc_connect();
    if (fd < 0)
        return;
    dprintf(fd, "%s\n", line);
    close(fd);
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
    {"foreign", s_foreign, "xdg-foreign: export a toplevel, import it, parent a second window to it"},
    {"evil", s_evil, "stale acks, commits between configures, self-resize, destroy/recreate"},
    {NULL, NULL, NULL},
};
