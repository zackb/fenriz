// Shared boilerplate for the fenriz-test clients: connect, bind, allocate shm, put a
// toplevel or popup on screen. Everything that talks to the display goes through
// wlc_dispatch/wlc_roundtrip, which turn a protocol error into a diagnostic exit.
#ifndef FENRIZ_WLCLIENT_H
#define FENRIZ_WLCLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

// Exit codes the runner distinguishes.
#define WLC_EXIT_FAIL 1    // protocol error, or the compositor went away
#define WLC_EXIT_TIMEOUT 2 // watchdog fired

extern bool wlc_verbose;

struct wlc {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    struct wl_subcompositor* subcompositor;
    struct wl_shm* shm;
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_data_device_manager* ddm;
    struct wl_data_device* data_device;
    struct xdg_wm_base* wm_base;
    struct zxdg_decoration_manager_v1* decoration;
    struct wp_viewporter* viewporter;
    struct wp_fractional_scale_manager_v1* frac_scale;
    struct zwlr_layer_shell_v1* layer_shell;
    struct zwlr_virtual_pointer_manager_v1* vpm;
    struct zwlr_virtual_pointer_v1* vp; // created by wlc_pointer_init

    // Last serial seen on any input event; xdg_popup grabs and start_drag need a real one.
    uint32_t last_serial;
    // Last wl_pointer.enter, so a scenario can assert where the pointer landed and how far
    // into that surface — which is how a window's on-screen position becomes observable to a
    // client that is never told its own coordinates.
    struct wl_surface* enter_surface;
    int enter_sx, enter_sy;
    int enters;
    // Cursor position the injected pointer has been walked to, in layout coordinates.
    int px, py;
    struct wl_output* outputs[8];
    int n_outputs;
    int out_w, out_h; // first output's current mode
};

struct win {
    struct wlc* c;
    struct wl_surface* surface;
    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* toplevel;
    struct xdg_popup* popup;
    struct xdg_positioner* positioner; // kept alive for reposition()
    struct zxdg_toplevel_decoration_v1* deco;
    struct zwlr_layer_surface_v1* layer; // set instead of toplevel for a layer surface

    int width, height;         // what we paint
    int cfg_width, cfg_height; // what the last configure asked for (0 = free choice)
    int cfg_x, cfg_y;          // where xdg_popup.configure placed us, in parent surface coords
    uint32_t last_configure_serial;
    uint32_t prev_configure_serial; // for acking a stale serial on purpose
    int configures;                 // xdg_surface.configure count
    bool acked;                     // acked the most recent configure
    bool configured;                // has seen at least one configure
    bool closed;                    // toplevel.close / popup.popup_done
    bool fullscreen, maximized, activated;

    // wl_output enter/leave bookkeeping for the hotplug scenario.
    struct wl_output* entered[8];
    int n_entered;
};

struct wlc* wlc_connect(void);
void wlc_finish(struct wlc* c);

// Dispatch one batch of events. Aborts the process on protocol error or disconnect.
void wlc_dispatch(struct wlc* c);
void wlc_roundtrip(struct wlc* c);
// Dispatch until `pred(arg)` is true. The watchdog is what breaks the loop if it never is.
void wlc_until(struct wlc* c, bool (*pred)(void*), void* arg);
// Dispatch for approximately `ms`, so timed scenarios still service events.
void wlc_pump(struct wlc* c, int ms);

struct win* wlc_toplevel(struct wlc* c, int w, int h, const char* title);
// A layer surface anchored to all four edges of the first output, so its configured
// size is the output size. Null if the compositor has no layer shell.
struct win* wlc_layer(struct wlc* c, const char* ns, uint32_t layer);
// x/y are relative to the parent's window geometry; grab uses the last input serial.
// The parent may be a toplevel or a layer surface.
struct win* wlc_popup(struct win* parent, int x, int y, int w, int h, bool grab);
void wlc_destroy(struct win* w);

// Ack the pending configure (if any), attach a freshly filled buffer, damage, commit.
void wlc_paint(struct win* w, uint32_t argb);
// Attach+commit without acking, for clients that are being difficult on purpose.
void wlc_paint_noack(struct win* w, uint32_t argb);
// Attach a buffer of a size we chose, which may contradict the configure we acked.
void wlc_paint_size(struct win* w, uint32_t argb, int bw, int bh);
void wlc_ack(struct win* w, uint32_t serial);
void wlc_wait_configure(struct win* w);
// Map = paint, then wait for the compositor to acknowledge us with a second configure.
void wlc_map(struct win* w, uint32_t argb);

struct wl_buffer* wlc_buffer(struct wlc* c, int w, int h, uint32_t argb);

// Injected pointer, via the virtual-pointer protocol: the headless backend has no input
// devices, so without this nothing can produce the button press that a drag grab.
void wlc_pointer_init(struct wlc* c);
void wlc_pointer_to(struct wlc* c, int x, int y);
void wlc_pointer_button(struct wlc* c, uint32_t button, bool pressed);

// Run an outright protocol violation in a throwaway child on its own connection. The
// child is expected to be killed; the assertion is that `c` is still live afterwards.
void wlc_abuse(struct wlc* c, const char* what, void (*fn)(struct wlc*));

// With --hold, park here with everything mapped so the surfaces can be looked at.
extern bool wlc_hold;
void wlc_hold_point(struct wlc* c);

// Watchdog. wlc_phase names what we're waiting on so a hang says where it hung.
void wlc_watchdog(int seconds);
void wlc_phase(const char* fmt, ...);
void wlc_log(const char* fmt, ...);
void wlc_die(const char* fmt, ...);

#endif
