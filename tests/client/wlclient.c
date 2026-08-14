#define _GNU_SOURCE
#include "wlclient.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool wlc_verbose = false;

static char phase[128] = "startup";

void wlc_log(const char* fmt, ...) {
    if (!wlc_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("  . ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void wlc_die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n      (during: %s)\n", phase);
    va_end(ap);
    exit(WLC_EXIT_FAIL);
}

void wlc_phase(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(phase, sizeof(phase), fmt, ap);
    va_end(ap);
    if (wlc_verbose)
        fprintf(stderr, "  > %s\n", phase);
}

static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "TIMEOUT: stuck waiting on: ";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    r = write(2, phase, strlen(phase));
    r = write(2, "\n", 1);
    (void)r;
    _exit(WLC_EXIT_TIMEOUT);
}

void wlc_watchdog(int seconds) {
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, NULL);
    alarm(seconds);
}

// --- error gate -----------------------------------------------------------------------

// Set only inside a wlc_abuse child, where being killed is the expected outcome.
#define ABUSE_REJECTED 3
static bool expect_error = false;

static void check_error(struct wlc* c) {
    int err = wl_display_get_error(c->display);
    if (!err)
        return;
    const struct wl_interface* iface = NULL;
    uint32_t id = 0, code = 0;
    if (err == EPROTO)
        code = wl_display_get_protocol_error(c->display, &iface, &id);
    if (expect_error) {
        fprintf(stderr, "      rejected with %s#%u code=%u\n", iface ? iface->name : "?", id, code);
        _exit(ABUSE_REJECTED);
    }
    if (err == EPROTO)
        wlc_die("protocol error: %s#%u code=%u", iface ? iface->name : "?", id, code);
    wlc_die("compositor disconnected: %s", strerror(err));
}

void wlc_dispatch(struct wlc* c) {
    if (wl_display_dispatch(c->display) < 0)
        check_error(c);
    check_error(c);
}

void wlc_roundtrip(struct wlc* c) {
    if (wl_display_roundtrip(c->display) < 0)
        check_error(c);
    check_error(c);
}

void wlc_until(struct wlc* c, bool (*pred)(void*), void* arg) {
    while (!pred(arg))
        wlc_dispatch(c);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void wlc_pump(struct wlc* c, int ms) {
    struct pollfd p = {.fd = wl_display_get_fd(c->display), .events = POLLIN};
    int64_t end = now_ms() + ms;
    for (;;) {
        if (wl_display_dispatch_pending(c->display) < 0)
            check_error(c);
        check_error(c);
        wl_display_flush(c->display);
        int left = (int)(end - now_ms());
        if (left <= 0)
            break;
        if (poll(&p, 1, left) > 0)
            wlc_dispatch(c);
    }
}

// --- shm buffers ----------------------------------------------------------------------

struct buf {
    void* data;
    size_t size;
};

static void buffer_release(void* data, struct wl_buffer* wl_buffer) {
    struct buf* b = data;
    munmap(b->data, b->size);
    free(b);
    wl_buffer_destroy(wl_buffer);
}
static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

struct wl_buffer* wlc_buffer(struct wlc* c, int w, int h, uint32_t argb) {
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    size_t stride = (size_t)w * 4, size = stride * h;
    int fd = memfd_create("fenriz-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0)
        wlc_die("memfd: %s", strerror(errno));
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
        wlc_die("mmap: %s", strerror(errno));
    for (size_t i = 0; i < size / 4; i++)
        ((uint32_t*)data)[i] = argb;

    struct wl_shm_pool* pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    struct buf* meta = calloc(1, sizeof *meta);
    meta->data = data;
    meta->size = size;
    wl_buffer_add_listener(b, &buffer_listener, meta);
    return b;
}

// --- listeners ------------------------------------------------------------------------

static void wm_base_ping(void* data, struct xdg_wm_base* b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void xdg_surface_configure(void* data, struct xdg_surface* s, uint32_t serial) {
    (void)s;
    struct win* w = data;
    w->prev_configure_serial = w->last_configure_serial;
    w->last_configure_serial = serial;
    w->configures++;
    w->configured = true;
    w->acked = false;
    wlc_log("configure #%d serial=%u size=%dx%d", w->configures, serial, w->cfg_width, w->cfg_height);
}
static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_configure};

static void
    toplevel_configure(void* data, struct xdg_toplevel* t, int32_t width, int32_t height, struct wl_array* states) {
    (void)t;
    struct win* w = data;
    w->cfg_width = width;
    w->cfg_height = height;
    w->fullscreen = w->maximized = w->activated = false;
    uint32_t* st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN)
            w->fullscreen = true;
        else if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED)
            w->maximized = true;
        else if (*st == XDG_TOPLEVEL_STATE_ACTIVATED)
            w->activated = true;
    }
}
static void toplevel_close(void* data, struct xdg_toplevel* t) {
    (void)t;
    ((struct win*)data)->closed = true;
}
static void toplevel_bounds(void* data, struct xdg_toplevel* t, int32_t w, int32_t h) {
    (void)data;
    (void)t;
    (void)w;
    (void)h;
}
static void toplevel_caps(void* data, struct xdg_toplevel* t, struct wl_array* caps) {
    (void)data;
    (void)t;
    (void)caps;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_caps,
};

static void popup_configure(void* data, struct xdg_popup* p, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)p;
    struct win* w = data;
    w->cfg_x = x;
    w->cfg_y = y;
    w->cfg_width = width;
    w->cfg_height = height;
    wlc_log("popup configure at %d,%d %dx%d", x, y, width, height);
}
static void popup_done(void* data, struct xdg_popup* p) {
    (void)p;
    ((struct win*)data)->closed = true;
    wlc_log("popup_done");
}
static void popup_repositioned(void* data, struct xdg_popup* p, uint32_t token) {
    (void)data;
    (void)p;
    wlc_log("popup repositioned token=%u", token);
}
static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void surface_enter(void* data, struct wl_surface* s, struct wl_output* out) {
    (void)s;
    struct win* w = data;
    for (int i = 0; i < w->n_entered; i++)
        if (w->entered[i] == out)
            wlc_die("wl_surface.enter for an output we are already on");
    if (w->n_entered < 8)
        w->entered[w->n_entered++] = out;
    wlc_log("surface enter output %p (now on %d)", (void*)out, w->n_entered);
}
static void surface_leave(void* data, struct wl_surface* s, struct wl_output* out) {
    (void)s;
    struct win* w = data;
    for (int i = 0; i < w->n_entered; i++) {
        if (w->entered[i] == out) {
            w->entered[i] = w->entered[--w->n_entered];
            wlc_log("surface leave output %p (now on %d)", (void*)out, w->n_entered);
            return;
        }
    }
    wlc_die("wl_surface.leave for an output we never entered");
}
static void surface_pref_scale(void* data, struct wl_surface* s, int32_t factor) {
    (void)data;
    (void)s;
    wlc_log("preferred_buffer_scale %d", factor);
}
static void surface_pref_transform(void* data, struct wl_surface* s, uint32_t transform) {
    (void)data;
    (void)s;
    (void)transform;
}
static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_pref_scale,
    .preferred_buffer_transform = surface_pref_transform,
};

// Input serials. Popup grabs and start_drag want one that the seat considers valid.
static void
    pointer_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    struct wlc* c = d;
    c->last_serial = serial;
    c->enter_surface = s;
    c->enter_sx = wl_fixed_to_int(x);
    c->enter_sy = wl_fixed_to_int(y);
    c->enters++;
}
static void pointer_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {
    (void)p;
    (void)s;
    ((struct wlc*)d)->last_serial = serial;
}
static void pointer_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)p;
    (void)t;
    (void)x;
    (void)y;
}
static void pointer_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state) {
    (void)p;
    (void)t;
    (void)b;
    (void)state;
    ((struct wlc*)d)->last_serial = serial;
}
static void pointer_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d;
    (void)p;
    (void)t;
    (void)a;
    (void)v;
}
static void pointer_noop(void* d, struct wl_pointer* p) {
    (void)d;
    (void)p;
}
static void pointer_u32(void* d, struct wl_pointer* p, uint32_t v) {
    (void)d;
    (void)p;
    (void)v;
}
static void pointer_u32_i32(void* d, struct wl_pointer* p, uint32_t a, int32_t v) {
    (void)d;
    (void)p;
    (void)a;
    (void)v;
}
static void pointer_u32_u32(void* d, struct wl_pointer* p, uint32_t a, uint32_t b) {
    (void)d;
    (void)p;
    (void)a;
    (void)b;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_noop,
    .axis_source = pointer_u32,
    .axis_stop = pointer_u32_u32,
    .axis_discrete = pointer_u32_i32,
    .axis_value120 = pointer_u32_i32,
    .axis_relative_direction = pointer_u32_u32,
};

static void seat_caps(void* data, struct wl_seat* seat, uint32_t caps) {
    struct wlc* c = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !c->pointer) {
        c->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(c->pointer, &pointer_listener, c);
    }
}
static void seat_name(void* d, struct wl_seat* s, const char* n) {
    (void)d;
    (void)s;
    (void)n;
}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

// --- outputs --------------------------------------------------------------------------

// Only the first output's size is kept, and only so a scenario can aim the injected pointer
// at a window the compositor placed (a centered float) without knowing where that is.
static void output_geometry(void* d,
                            struct wl_output* o,
                            int32_t x,
                            int32_t y,
                            int32_t pw,
                            int32_t ph,
                            int32_t sp,
                            const char* m,
                            const char* mo,
                            int32_t t) {
    (void)d;
    (void)o;
    (void)x;
    (void)y;
    (void)pw;
    (void)ph;
    (void)sp;
    (void)m;
    (void)mo;
    (void)t;
}
static void output_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)refresh;
    struct wlc* c = d;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && c->n_outputs > 0 && c->outputs[0] == o) {
        c->out_w = w;
        c->out_h = h;
    }
}
static void output_done(void* d, struct wl_output* o) {
    (void)d;
    (void)o;
}
static void output_scale(void* d, struct wl_output* o, int32_t f) {
    (void)d;
    (void)o;
    (void)f;
}
static void output_str(void* d, struct wl_output* o, const char* s) {
    (void)d;
    (void)o;
    (void)s;
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_str,
    .description = output_str,
};

// --- registry -------------------------------------------------------------------------

static uint32_t cap(uint32_t have, uint32_t want) { return have < want ? have : want; }

static void registry_global(void* data, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    struct wlc* c = data;
    if (!strcmp(iface, wl_compositor_interface.name))
        c->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, cap(version, 6));
    else if (!strcmp(iface, wl_subcompositor_interface.name))
        c->subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name))
        c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_data_device_manager_interface.name))
        c->ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface, cap(version, 3));
    else if (!strcmp(iface, wl_seat_interface.name)) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface, cap(version, 7));
        wl_seat_add_listener(c->seat, &seat_listener, c);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        // v3 is the floor: xdg_popup.reposition lives there. Log what we actually got so a
        // version regression in fenriz shows up in --verbose runs.
        c->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, cap(version, 6));
        xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
        wlc_log("xdg_wm_base advertised v%u, bound v%u", version, cap(version, 6));
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
        c->decoration = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_viewporter_interface.name))
        c->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name))
        c->frac_scale = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        c->vpm = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, cap(version, 2));
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        c->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, cap(version, 4));
    else if (!strcmp(iface, wl_output_interface.name) && c->n_outputs < 8) {
        c->outputs[c->n_outputs++] = wl_registry_bind(reg, name, &wl_output_interface, cap(version, 4));
        wl_output_add_listener(c->outputs[c->n_outputs - 1], &output_listener, c);
    }
}
static void registry_remove(void* data, struct wl_registry* reg, uint32_t name) {
    (void)data;
    (void)reg;
    wlc_log("global %u removed", name);
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

struct wlc* wlc_connect(void) {
    struct wlc* c = calloc(1, sizeof *c);
    wlc_phase("connecting to WAYLAND_DISPLAY=%s", getenv("WAYLAND_DISPLAY") ?: "(unset)");
    c->display = wl_display_connect(NULL);
    if (!c->display)
        wlc_die("wl_display_connect: %s (WAYLAND_DISPLAY=%s)", strerror(errno), getenv("WAYLAND_DISPLAY") ?: "(unset)");
    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    wlc_roundtrip(c); // globals
    wlc_roundtrip(c); // seat capabilities, output geometry

    if (!c->compositor || !c->shm || !c->wm_base)
        wlc_die("compositor is missing wl_compositor, wl_shm or xdg_wm_base");
    if (c->ddm && c->seat)
        c->data_device = wl_data_device_manager_get_data_device(c->ddm, c->seat);
    return c;
}

void wlc_finish(struct wlc* c) {
    wlc_roundtrip(c);
    wl_display_disconnect(c->display);
    free(c);
}

// --- injected pointer -----------------------------------------------------------------

// Event timestamps, in ms. Only ever compared to each other, so start from zero.
static uint32_t vp_time = 1;

static void vp_frame(struct wlc* c) {
    zwlr_virtual_pointer_v1_frame(c->vp);
    wlc_roundtrip(c);
}

void wlc_pointer_init(struct wlc* c) {
    if (!c->vpm)
        wlc_die("compositor does not advertise zwlr_virtual_pointer_manager_v1");
    if (c->vp)
        return;
    c->vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(c->vpm, c->seat);
    wlc_roundtrip(c);
    // wlr_cursor clamps a warp to the output layout, so one enormous negative step parks the
    // cursor on the layout's top-left corner whatever the outputs are. Everything after this
    // is a known delta from a known origin.
    zwlr_virtual_pointer_v1_motion(c->vp, vp_time++, wl_fixed_from_int(-1000000), wl_fixed_from_int(-1000000));
    vp_frame(c);
    c->px = c->py = 0;
}

void wlc_pointer_to(struct wlc* c, int x, int y) {
    if (!c->vp)
        wlc_pointer_init(c);
    zwlr_virtual_pointer_v1_motion(c->vp, vp_time++, wl_fixed_from_int(x - c->px), wl_fixed_from_int(y - c->py));
    c->px = x;
    c->py = y;
    vp_frame(c);
}

void wlc_pointer_button(struct wlc* c, uint32_t button, bool pressed) {
    if (!c->vp)
        wlc_pointer_init(c);
    zwlr_virtual_pointer_v1_button(
        c->vp, vp_time++, button, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    vp_frame(c);
}

// --- windows --------------------------------------------------------------------------

struct win* wlc_toplevel(struct wlc* c, int w, int h, const char* title) {
    struct win* t = calloc(1, sizeof *t);
    t->c = c;
    t->width = w;
    t->height = h;
    t->surface = wl_compositor_create_surface(c->compositor);
    wl_surface_add_listener(t->surface, &surface_listener, t);
    t->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, t->surface);
    xdg_surface_add_listener(t->xdg_surface, &xdg_surface_listener, t);
    t->toplevel = xdg_surface_get_toplevel(t->xdg_surface);
    xdg_toplevel_add_listener(t->toplevel, &toplevel_listener, t);
    xdg_toplevel_set_title(t->toplevel, title);
    xdg_toplevel_set_app_id(t->toplevel, "fenriz-test");
    // fenriz forces SSD; ask for it explicitly so the negotiated geometry is exercised.
    if (c->decoration) {
        t->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(c->decoration, t->toplevel);
        zxdg_toplevel_decoration_v1_set_mode(t->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(t->surface); // initial commit: no buffer, per xdg-shell
    return t;
}

// A layer surface acks its own configures: it has no xdg_surface for wlc_ack to use,
// and marking it acked+configured is what lets wlc_paint/wlc_map treat it like any
// other window.
static void
    layer_configure(void* data, struct zwlr_layer_surface_v1* l, uint32_t serial, uint32_t width, uint32_t height) {
    struct win* w = data;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    if (width > 0)
        w->width = (int)width;
    if (height > 0)
        w->height = (int)height;
    w->configured = true;
    w->acked = true;
    wlc_log("layer configure %ux%u", width, height);
}
static void layer_closed(void* data, struct zwlr_layer_surface_v1* l) {
    (void)l;
    ((struct win*)data)->closed = true;
    wlc_log("layer closed");
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

struct win* wlc_layer(struct wlc* c, const char* ns, uint32_t layer) {
    if (!c->layer_shell)
        return NULL;
    struct win* l = calloc(1, sizeof *l);
    l->c = c;
    l->surface = wl_compositor_create_surface(c->compositor);
    wl_surface_add_listener(l->surface, &surface_listener, l);
    l->layer = zwlr_layer_shell_v1_get_layer_surface(
        c->layer_shell, l->surface, c->n_outputs > 0 ? c->outputs[0] : NULL, layer, ns);
    zwlr_layer_surface_v1_add_listener(l->layer, &layer_listener, l);
    // Anchored to all four edges with size 0x0, so the compositor hands back the
    // output size and the tests never hardcode a resolution.
    zwlr_layer_surface_v1_set_anchor(l->layer,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(l->layer, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(l->layer, -1);
    wl_surface_commit(l->surface); // initial commit: no buffer
    return l;
}

struct win* wlc_popup(struct win* parent, int x, int y, int w, int h, bool grab) {
    struct wlc* c = parent->c;
    struct win* p = calloc(1, sizeof *p);
    p->c = c;
    p->width = w;
    p->height = h;
    p->positioner = xdg_wm_base_create_positioner(c->wm_base);
    xdg_positioner_set_size(p->positioner, w, h);
    xdg_positioner_set_anchor_rect(p->positioner, x, y, 1, 1);
    xdg_positioner_set_anchor(p->positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(p->positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(
        p->positioner,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    p->surface = wl_compositor_create_surface(c->compositor);
    wl_surface_add_listener(p->surface, &surface_listener, p);
    p->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, p->surface);
    xdg_surface_add_listener(p->xdg_surface, &xdg_surface_listener, p);
    // A layer-shell parent has no xdg_surface: the popup is created parentless and
    // adopted by the layer surface, per wlr-layer-shell.
    p->popup = xdg_surface_get_popup(p->xdg_surface, parent->layer ? NULL : parent->xdg_surface, p->positioner);
    xdg_popup_add_listener(p->popup, &popup_listener, p);
    if (parent->layer)
        zwlr_layer_surface_v1_get_popup(parent->layer, p->popup);
    if (grab && c->seat)
        xdg_popup_grab(p->popup, c->seat, c->last_serial);
    wl_surface_commit(p->surface);
    return p;
}

void wlc_destroy(struct win* w) {
    if (!w)
        return;
    if (w->deco)
        zxdg_toplevel_decoration_v1_destroy(w->deco);
    if (w->popup)
        xdg_popup_destroy(w->popup);
    if (w->toplevel)
        xdg_toplevel_destroy(w->toplevel);
    if (w->layer)
        zwlr_layer_surface_v1_destroy(w->layer);
    if (w->positioner)
        xdg_positioner_destroy(w->positioner);
    if (w->xdg_surface)
        xdg_surface_destroy(w->xdg_surface);
    if (w->surface)
        wl_surface_destroy(w->surface);
    free(w);
}

void wlc_ack(struct win* w, uint32_t serial) {
    xdg_surface_ack_configure(w->xdg_surface, serial);
    w->acked = true;
}

static bool is_configured(void* arg) { return ((struct win*)arg)->configured; }

void wlc_wait_configure(struct win* w) {
    wl_display_flush(w->c->display);
    wlc_until(w->c, is_configured, w);
}

void wlc_paint_size(struct win* w, uint32_t argb, int bw, int bh) {
    if (!w->acked && w->configured)
        wlc_ack(w, w->last_configure_serial);
    struct wl_buffer* b = wlc_buffer(w->c, bw, bh, argb);
    wl_surface_attach(w->surface, b, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, bw, bh);
    wl_surface_commit(w->surface);
}

void wlc_paint(struct win* w, uint32_t argb) {
    // Honour the configured size when the compositor picked one; that's what a
    // well-behaved client does, and a wrong one here would mask geometry bugs.
    if (w->cfg_width > 0 && w->cfg_height > 0) {
        w->width = w->cfg_width;
        w->height = w->cfg_height;
    }
    wlc_paint_size(w, argb, w->width, w->height);
}

void wlc_paint_noack(struct win* w, uint32_t argb) {
    struct wl_buffer* b = wlc_buffer(w->c, w->width, w->height, argb);
    wl_surface_attach(w->surface, b, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, w->width, w->height);
    wl_surface_commit(w->surface);
}

void wlc_map(struct win* w, uint32_t argb) {
    wlc_wait_configure(w);
    wlc_paint(w, argb);
    wlc_roundtrip(w->c);
}

// --- abuse ----------------------------------------------------------------------------

// Some of what we want to try is an outright protocol violation, and the compositor is supposed to kill the client for
// it.
void wlc_abuse(struct wlc* c, const char* what, void (*fn)(struct wlc*)) {
    wlc_phase("abuse: %s", what);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        wlc_die("fork: %s", strerror(errno));
    if (pid == 0) {
        alarm(10);
        struct wlc* child = wlc_connect();
        expect_error = true;
        fn(child);
        wl_display_flush(child->display);
        wl_display_roundtrip(child->display);
        check_error(child);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
    fprintf(stderr,
            "  abuse: %-45s %s\n",
            what,
            rc == ABUSE_REJECTED ? "rejected"
            : rc == 0            ? "ACCEPTED (compositor let it through)"
                                 : "child died unexpectedly");
    wlc_roundtrip(c); // the real check: are we still connected to a live compositor?
}

// --- hold -----------------------------------------------------------------------------

bool wlc_hold = false;

void wlc_hold_point(struct wlc* c) {
    if (!wlc_hold)
        return;
    alarm(0); // eyeballing has no deadline
    fprintf(stderr, "--hold: surfaces are up, ^C to finish\n");
    for (;;)
        wlc_pump(c, 1000);
}
