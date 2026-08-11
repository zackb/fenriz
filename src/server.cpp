#include "server.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cursor.hpp"
#include "decoration.hpp"
#include "ipc.hpp"
#include "keyboard.hpp"
#include "layer.hpp"
#include "lock.hpp"
#include "output.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"
#include "xwayland.hpp"

namespace fenriz {

    namespace {

        void on_new_toplevel(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            // View registers its own map/unmap/destroy listeners and deletes itself on
            // destroy, so the raw new is intentional (not a leak).
            new View(*sl->server, static_cast<wlr_xdg_toplevel*>(data));
        }

        // Per-popup state: popups need their own commit/destroy listeners, so each one gets a
        // small heap object, freed when the popup goes away.
        struct Popup {
            Server* server;
            wlr_xdg_popup* popup;
            wl_listener commit;
            wl_listener destroy;
            wl_listener reposition;
            wl_listener tree_destroy;
        };

        // Walk up through nested submenus to the xdg surface a popup chain hangs off.
        // Null ONLY when the chain escapes to a non-xdg surface
        wlr_xdg_surface* popup_root(wlr_xdg_surface* from) {
            wlr_xdg_surface* root = from;
            while (root->role == WLR_XDG_SURFACE_ROLE_POPUP) {
                wlr_xdg_surface* up =
                    root->popup->parent ? wlr_xdg_surface_try_from_wlr_surface(root->popup->parent) : nullptr;
                if (!up)
                    return nullptr;
                root = up;
            }
            return root;
        }

        // The mapped View owning `root`, or null. `mapped_only` additionally requires a live
        // scene tree — an unmapped toplevel leaves stale scene pointers behind in base->data.
        View* view_for_toplevel(Server& server, wlr_xdg_surface* root, bool mapped_only = false) {
            for (View* v : server.views)
                if (v->toplevel == root->toplevel && (!mapped_only || v->scene_tree))
                    return v;
            return nullptr;
        }

        // The box a popup must stay inside, in the root toplevel's window-geometry coordinate
        // space (what wlr_xdg_popup_unconstrain_from_box wants). False when the popup isn't
        // owned by a mapped View (layer-shell root), or a window that's unmapped/homeless
        bool popup_constraint_box(Server& server, wlr_xdg_popup* popup, wlr_box* out) {
            wlr_xdg_surface* root = popup_root(popup->base);
            if (!root || root->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
                return false; // layer-shell root: not a coordinate space we can work out

            View* view = view_for_toplevel(server, root);
            output::Output* o = view ? view_output(server, view) : nullptr;
            if (!o)
                return false;

            // usable_area is layout coords with the bars' exclusive zones removed, so menus stay clear of them.
            const int bw = view->fullscreen ? 0 : server.config.border_width;
            // `frame`, not `box`: a client that refuses its tile size is drawn centered in it,
            // and popups anchor to where the window actually is.
            *out = {o->usable_area.x - (view->frame.x + bw) + root->geometry.x,
                    o->usable_area.y - (view->frame.y + bw) + root->geometry.y,
                    o->usable_area.width,
                    o->usable_area.height};
            return true;
        }

        // Flip/slide the popup back on-screen against the constraint box. Shared by the
        // initial-commit and reposition paths. unconstrain_from_box schedules the configure
        // (carrying the reposition token on the reposition path), so callers needn't.
        void unconstrain_popup(Server& server, wlr_xdg_popup* popup) {
            if (wlr_box box; popup_constraint_box(server, popup, &box))
                wlr_xdg_popup_unconstrain_from_box(popup, &box);
        }

        void on_popup_commit(wl_listener* listener, void* data) {
            Popup* p = wl_container_of(listener, p, commit);
            (void)data;
            if (!p->popup->base->initial_commit)
                return;
            // The initial commit must be answered with a configure or the client never maps the
            // popup: it waits forever, attaches no buffer, and nothing is ever drawn (the
            // toplevel path does the same in view_handle_commit). Unconstraining schedules a
            // configure itself, but not on the path where we can't place the popup so ask for
            // one unconditionally. It's idempotent; wlroots dedups via configure_idle.
            unconstrain_popup(*p->server, p->popup);
            wlr_xdg_surface_schedule_configure(p->popup->base);
        }

        // GTK/Firefox clients reuse one popover surface and reposition it each time it's
        // shown; wlroots applies the new positioner but leaves unconstraining to us.
        void on_popup_reposition(wl_listener* listener, void* data) {
            Popup* p = wl_container_of(listener, p, reposition);
            (void)data;
            unconstrain_popup(*p->server, p->popup);
            wlr_xdg_surface_schedule_configure(p->popup->base);
        }

        // The popup's scene tree was freed. base->data still points at it, and wlroots never
        // invalidates that field, so null it here.
        void on_popup_tree_destroy(wl_listener* listener, void* data) {
            Popup* p = wl_container_of(listener, p, tree_destroy);
            (void)data;
            p->popup->base->data = nullptr;
            wl_list_remove(&p->tree_destroy.link);
            wl_list_init(&p->tree_destroy.link); // on_popup_destroy removes it again
        }

        void on_popup_destroy(wl_listener* listener, void* data) {
            Popup* p = wl_container_of(listener, p, destroy);
            (void)data;
            wl_list_remove(&p->commit.link);
            wl_list_remove(&p->destroy.link);
            wl_list_remove(&p->reposition.link);
            wl_list_remove(&p->tree_destroy.link);
            delete p;
        }

        // The parent's scene tree is stashed in xdg_surface->data (by view map, and by
        // popup_create for nested popups). A popup whose parent IS the layer surface has a
        // non-xdg parent at this point and is handled in layer.cpp; its own submenus come
        // back through here with a popup parent.
        void on_new_popup(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* popup = static_cast<wlr_xdg_popup*>(data);
            if (!popup->parent)
                return;
            wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
            if (!parent || !parent->data)
                return;
            // null root means the chain ends at a layer surface (menu's submenu).
            wlr_xdg_surface* root = popup_root(parent);
            if (root && root->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
                View* owner = view_for_toplevel(*sl->server, root, /*mapped_only=*/true);
                if (!owner)
                    return; // toplevel unmapped: parent->data is freed
                // Popups live inside the owner's scene subtree, so they can't rise above a
                // sibling tiled toplevel. Raise the owner's tree.
                wlr_scene_node_raise_to_top(&owner->scene_tree->node);
            }
            popup_create(*sl->server, popup, static_cast<wlr_scene_tree*>(parent->data));
        }

        void on_new_input(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            handle_new_input(*sl->server, static_cast<wlr_input_device*>(data));
        }

        // inotify fired on the config directory: if fenriz.conf was (re)written, hot-reload.
        int on_config_changed(int fd, uint32_t mask, void* data) {
            (void)mask;
            auto* server = static_cast<Server*>(data);
            // Drain the queue; an aligned buffer big enough for at least one full event.
            alignas(inotify_event) char buf[4096];
            bool hit = false;
            for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) {
                for (char* p = buf; p < buf + n;) {
                    auto* ev = reinterpret_cast<inotify_event*>(p);
                    if (ev->len && std::strcmp(ev->name, "fenriz.conf") == 0)
                        hit = true;
                    p += sizeof(inotify_event) + ev->len;
                }
            }
            if (hit)
                reload_config(*server);
            return 0;
        }

        // Watch the config directory
        void init_config_watch(Server& server, wl_event_loop* loop) {
            std::string path = Config::config_path();
            if (path.empty())
                return;
            std::string dir = path.substr(0, path.find_last_of('/'));
            int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd < 0)
                return;
            if (inotify_add_watch(fd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
                close(fd);
                return;
            }
            server.config_watch = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, on_config_changed, &server);
            if (!server.config_watch) {
                close(fd);
                return;
            }
            wlr_log(WLR_INFO, "fenriz: watching %s for changes", path.c_str());
        }

        // Clipboard: a client with keyboard focus asks to own the selection. Honor it so
        // copy/paste works between clients. Same shape for the primary (middle-click) one.
        void on_set_selection(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_seat_request_set_selection_event*>(data);
            wlr_seat_set_selection(sl->server->seat, ev->source, ev->serial);
        }

        void on_set_primary_selection(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
            wlr_seat_set_primary_selection(sl->server->seat, ev->source, ev->serial);
        }

        void on_drag_icon_destroy(wl_listener* listener, void*) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            sl->server->drag_icon = nullptr; // scene node is freed by wlroots
            wl_list_remove(&sl->listener.link);
            wl_list_init(&sl->listener.link);
        }

        void on_request_start_drag(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_seat_request_start_drag_event*>(data);
            wlr_seat* seat = sl->server->seat;
            if (!wlr_seat_validate_pointer_grab_serial(seat, ev->origin, ev->serial)) {
                if (ev->drag->source)
                    wlr_data_source_destroy(ev->drag->source);
                return;
            }
            wlr_seat_start_pointer_drag(seat, ev->drag, ev->serial);
            // Render the drag icon: wire it into the scene above windows and let cursor motion
            // (process_motion) track it. wlroots frees the node when the icon is destroyed.
            if (ev->drag->icon) {
                Server* s = sl->server;
                if (s->drag_icon) {
                    wl_list_remove(&s->l_drag_icon_destroy.listener.link);
                    s->drag_icon = nullptr;
                }
                wlr_scene_tree* icon = wlr_scene_drag_icon_create(s->scene_overlay, ev->drag->icon);
                if (!icon)
                    return; // nothing to render or track
                s->drag_icon = icon;
                s->l_drag_icon_destroy.server = s;
                add_listener(s->l_drag_icon_destroy.listener, ev->drag->icon->events.destroy, on_drag_icon_destroy);
            }
        }

        void on_set_gamma(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_gamma_control_manager_v1_set_gamma_event*>(data);
            // Applied on the next frame (output.cpp); mark dirty + wake the output so the
            // frame handler commits even though the scene itself needs no repaint.
            if (output::Output* o = output::by_handle(*sl->server, ev->output))
                o->gamma_dirty = true;
            wlr_output_schedule_frame(ev->output);
        }

        // ext-image-copy-capture: a portal (xdg-desktop-portal-wlr) asks to capture one
        // window. Map the ext-foreign-toplevel handle back to our View and hand the portal a
        // capture source — this is what enables per-window screen sharing
        void on_new_ext_capture_request(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* req = static_cast<wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request*>(data);
            Server& server = *sl->server;
            for (View* v : server.views) {
                if (v->ext_foreign_handle != req->toplevel_handle)
                    continue;
                wlr_surface* surface = view_surface(v);
                if (!surface)
                    break; // matched but nothing to mirror yet; let the request lapse
                if (!v->capture_source) {
                    v->capture_scene = wlr_scene_create();
                    wlr_scene_subsurface_tree_create(&v->capture_scene->tree, surface);
                    v->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
                        &v->capture_scene->tree.node,
                        wl_display_get_event_loop(server.display),
                        server.allocator,
                        server.renderer);
                }
                wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req, v->capture_source);
                return;
            }
            // Unknown/unmapped handle: don't accept; wlroots hands the client an inert source.
        }

        // wlr-output-power-management: a shell/idle daemon (wlopm, hypridle) toggles DPMS.
        // Shares set_dpms with the IPC `dpms` command. The request names an output — honor it
        // rather than blanking whichever one happens to be first.
        void on_output_power(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_output_power_v1_set_mode_event*>(data);
            // The request names one output; a stale/unknown handle must be a no-op, not fall
            // through to set_dpms's null-means-every-output path and blank all screens.
            output::Output* o = output::by_handle(*sl->server, ev->output);
            if (!o)
                return;
            output::set_dpms(*sl->server, o, ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
        }

        // xdg-activation-v1: a client asks for a window to be raised. Mark the
        // window urgent and let the shell paint its workspace. Focusing it
        // clears the flag.
        void on_activation_request(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* ev = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
            Server& s = *sl->server;
            for (View* v : s.views) {
                if (view_surface(v) != ev->surface)
                    continue;
                // Already looking at it: nothing to demand attention about.
                if (v == s.focused_view || view_visible(s, v))
                    return;
                v->urgent = true;
                ipc::publish(s);
                return;
            }
        }

        // idle-inhibit-v1: a client holding an inhibitor (video/fullscreen) keeps the
        // screen awake by suppressing the idle notifier that ext-idle-notify feeds.
        struct IdleInhibitor {
            wl_listener destroy;
            Server* server;
        };

        void on_inhibitor_destroy(wl_listener* listener, void*) {
            IdleInhibitor* ii = wl_container_of(listener, ii, destroy);
            Server* s = ii->server;
            s->active_inhibitors--;
            wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, s->active_inhibitors > 0);
            wl_list_remove(&ii->destroy.link);
            delete ii;
        }

        void on_new_idle_inhibitor(wl_listener* listener, void* data) {
            SignalListener* sl = wl_container_of(listener, sl, listener);
            auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
            auto* ii = new IdleInhibitor{};
            ii->server = sl->server;
            add_listener(ii->destroy, inhibitor->events.destroy, on_inhibitor_destroy);
            sl->server->active_inhibitors++;
            wlr_idle_notifier_v1_set_inhibited(sl->server->idle_notifier, true);
        }

    } // namespace

    // Parent a popup into `parent_tree` so it renders above its parent and tracks its
    // position, and take responsibility for configuring it.
    void popup_create(Server& server, wlr_xdg_popup* popup, wlr_scene_tree* parent_tree) {
        wlr_scene_tree* tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
        popup->base->data = tree;
        Popup* p = new Popup{&server, popup, {}, {}, {}, {}};
        add_listener(p->commit, popup->base->surface->events.commit, on_popup_commit);
        add_listener(p->destroy, popup->events.destroy, on_popup_destroy);
        add_listener(p->reposition, popup->events.reposition, on_popup_reposition);
        if (tree)
            add_listener(p->tree_destroy, tree->node.events.destroy, on_popup_tree_destroy);
        else
            wl_list_init(&p->tree_destroy.link); // on_popup_destroy removes it unconditionally
    }

    void spawn(const std::string& cmd) {
        if (cmd.empty())
            return;
        // Double-fork so the command reparents to init and fenriz never has a long-lived
        // child to reap. We deliberately do NOT set SIGCHLD to SIG_IGN: that disposition
        // survives execve and would leak into wlroots' Xwayland, whose keymap compile
        // wait4()s its xkbcomp child
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            if (fork() == 0) {
                execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), (char*)nullptr);
                _exit(127);
            }
            _exit(0);
        }
        if (pid > 0)
            waitpid(pid, nullptr, 0); // reap the intermediate; grandchild is init's now
    }

    Server::Server() { config = Config::load(); }

    Server::~Server() {
        if (config_watch)
            wl_event_source_remove(config_watch);
        ipc::shutdown();
        if (display) {
            wl_display_destroy_clients(display);
        }
    }

    bool Server::start() {

        display = wl_display_create();
        // libwayland caps each client's outgoing connection buffer at 4 KiB by default, and
        // kills the client outright when an event doesn't fit
        wl_display_set_default_max_buffer_size(display, 1024 * 1024);
        wl_event_loop* loop = wl_display_get_event_loop(display);

        backend = wlr_backend_autocreate(loop, &session);
        if (!backend) {
            wlr_log(WLR_ERROR, "failed to create backend");
            return false;
        }

        // SceneFX's GLES2 renderer (drop-in for wlr_renderer_autocreate); its shaders
        // are what draw the rounded corners / opacity set in place_view_nodes.
        renderer = fx_renderer_create(backend);
        if (!renderer) {
            wlr_log(WLR_ERROR, "failed to create renderer");
            return false;
        }
        // wl_shm only. The dmabuf global is created explicitly below at the version we want
        // wlr_renderer_init_wl_display would add a second one at version 4
        wlr_renderer_init_wl_shm(renderer, display);

        allocator = wlr_allocator_autocreate(backend, renderer);
        if (!allocator) {
            wlr_log(WLR_ERROR, "failed to create allocator");
            return false;
        }

        // v6 adds wl_surface.preferred_buffer_scale/transform: the compositor tells each
        // surface the scale to render at, instead of the client inferring it from the
        // wl_outputs it happens to overlap. Better HiDPI for toolkits that honor it.
        compositor = wlr_compositor_create(display, 6, renderer); // stored for wlr_xwayland_create
        wlr_subcompositor_create(display);
        wlr_data_device_manager_create(display);

        // HiDPI: viewporter + fractional-scale let clients submit correctly-sized buffers
        // for a fractional output scale (see config.scale / layer::arrange rendering).
        wlr_viewporter_create(display);
        wlr_fractional_scale_manager_v1_create(display, 1);

        output_layout = wlr_output_layout_create(display);

        // Scene graph must exist before outputs connect (handle_new_output creates a
        // scene-output per wlr_output). Trees are created in bottom -> top z-order.
        scene = wlr_scene_create();
        scene_layout = wlr_scene_attach_output_layout(scene, output_layout);
        scene_background = wlr_scene_tree_create(&scene->tree);
        scene_bottom = wlr_scene_tree_create(&scene->tree);
        scene_tiles = wlr_scene_tree_create(&scene->tree);
        scene_floating = wlr_scene_tree_create(&scene->tree);
        scene_top = wlr_scene_tree_create(&scene->tree);
        scene_fullscreen = wlr_scene_tree_create(&scene->tree);
        // X11 override-redirect surfaces (menus/tooltips/dropdowns): above fullscreen so a
        // menu from a fullscreen X game still shows, below layer-shell overlays and the lock.
        scene_unmanaged = wlr_scene_tree_create(&scene->tree);
        scene_overlay = wlr_scene_tree_create(&scene->tree);
        scene_lock = wlr_scene_tree_create(&scene->tree);

        // linux-dmabuf lets GPU clients (QtQuick/quickshell, browsers) share their GPU
        // buffers zero-copy instead of falling back to SHM (a per-frame GPU->CPU->upload
        // treadmill that burns CPU on both sides). presentation-time gives clients accurate
        // frame pacing so they throttle to vblank instead of rendering continuously.
        wlr_linux_dmabuf_v1_create_with_renderer(display, 5, renderer);
        // Explicit sync (wp_linux_drm_syncobj_v1): only advertise when renderer AND
        // backend support wait/signal timelines
        {
            int drm_fd = wlr_renderer_get_drm_fd(renderer);
            if (drm_fd >= 0 && renderer->features.timeline && backend->features.timeline)
                wlr_linux_drm_syncobj_manager_v1_create(display, 1, drm_fd);
        }
        // deliberately NOT wlr_scene_set_linux_dmabuf_v1(scene, dmabuf) — that opts into
        // per-surface scanout feedback, and wlr_scene re-mints a format-table shm fd for every
        // scene_buffer on any scene change (e.g. a workspace switch), including surfaces that
        // never scan out (bars, wallpapers). A client that only drains its per-surface event
        // queue on swapbuffers never reads those events, so a static surface leaks the fd until
        // it hits RLIMIT_NOFILE and libwayland kills the connection. Cost of leaving this off is
        // fullscreen direct scanout; re-enable once clients stop leaking.
        wlr_presentation_create(display, backend, 2);
        wlr_single_pixel_buffer_manager_v1_create(display);
        wlr_content_type_manager_v1_create(display, 1);

        // Seed workspace homes before any output shows up, so the first monitor to appear
        // already claims the workspaces configured for it.
        for (int i = 0; i < WS_COUNT; i++)
            workspaces[i].home = config.ws_home[i];

        output::register_handlers(*this);

        // v4 for configure_bounds
        xdg_shell = wlr_xdg_shell_create(display, 4);
        l_new_toplevel.server = this;
        add_listener(l_new_toplevel.listener, xdg_shell->events.new_toplevel, on_new_toplevel);
        l_new_popup.server = this;
        add_listener(l_new_popup.listener, xdg_shell->events.new_popup, on_new_popup);

        seat = wlr_seat_create(display, "seat0");
        wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
        l_new_input.server = this;
        add_listener(l_new_input.listener, backend->events.new_input, on_new_input);

        // Clipboard / selection: data_device_manager (above) needs these seat handlers to
        // actually move selections between clients, plus the primary (middle-click) manager
        // and data-control (wl-clipboard / clipboard managers).
        l_set_selection.server = this;
        add_listener(l_set_selection.listener, seat->events.request_set_selection, on_set_selection);
        l_set_primary_selection.server = this;
        add_listener(
            l_set_primary_selection.listener, seat->events.request_set_primary_selection, on_set_primary_selection);
        l_start_drag.server = this;
        add_listener(l_start_drag.listener, seat->events.request_start_drag, on_request_start_drag);
        wlr_primary_selection_v1_device_manager_create(display);
        wlr_data_control_manager_v1_create(display);
        wlr_ext_data_control_manager_v1_create(display, 1);

        // Export configured env vars (QT_QPA_PLATFORMTHEME, XCURSOR_THEME) before anything reads the environment
        for (const auto& [name, value] : config.env)
            setenv(name.c_str(), value.c_str(), 1);

        // XWayland: managed X11 toplevels. Needs the compositor + seat (both live now), and
        // exports DISPLAY so exec-once X clients (run below) can find it.
        xwayland::setup(*this);

        // Let external tools see the display and windows, grab screenshots, tune gamma.
        wlr_xdg_output_manager_v1_create(display, output_layout);
        wlr_screencopy_manager_v1_create(display);
        foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(display);
        // ext-foreign-toplevel-list is the standardized successor, but list-only: no
        // activate/close/fullscreen requests. Taskbars that want to *act* on a window still
        // need the wlr protocol above, so both stay live and each view carries both handles.
        ext_foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(display, 1);

        // ext-image-copy-capture: modern successor to wlr-screencopy; coexists with it.
        // xdg-desktop-portal-wlr prefers this path, and the foreign-toplevel source below
        // is what enables *per-window* screen sharing (screencopy alone is output-only).
        wlr_ext_image_copy_capture_manager_v1_create(display, 1);          // copy engine
        wlr_ext_output_image_capture_source_manager_v1_create(display, 1); // one source per output
        ext_toplevel_capture = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(display, 1);
        l_new_ext_capture_request.server = this;
        add_listener(
            l_new_ext_capture_request.listener, ext_toplevel_capture->events.new_request, on_new_ext_capture_request);

        gamma_control_manager = wlr_gamma_control_manager_v1_create(display);
        l_set_gamma.server = this;
        add_listener(l_set_gamma.listener, gamma_control_manager->events.set_gamma, on_set_gamma);

        // DPMS control for shells/idle daemons (also reachable via the IPC `dpms` command).
        output_power_manager = wlr_output_power_manager_v1_create(display);
        l_output_power.server = this;
        add_listener(l_output_power.listener, output_power_manager->events.set_mode, on_output_power);

        // Windows asking to be raised; marks them urgent for the bar rather than stealing focus.
        xdg_activation = wlr_xdg_activation_v1_create(display);
        l_activation_request.server = this;
        add_listener(l_activation_request.listener, xdg_activation->events.request_activate, on_activation_request);

        cursor::init(*this);
        init_keyboard(*this); // virtual-keyboard + shortcuts-inhibit; needs the seat above

        layer::init(*this); // creates idle_notifier; must precede idle-inhibit wiring below
        lock::init(*this);
        decoration::init(*this);

        // idle-inhibit: keep the screen awake while a client holds an inhibitor. Wired
        // after layer::init so idle_notifier is non-null for the manager's whole life.
        idle_inhibit_manager = wlr_idle_inhibit_v1_create(display);
        l_new_idle_inhibitor.server = this;
        add_listener(l_new_idle_inhibitor.listener, idle_inhibit_manager->events.new_inhibitor, on_new_idle_inhibitor);

        const char* socket = wl_display_add_socket_auto(display);
        if (!socket) {
            wlr_log(WLR_ERROR, "failed to create wayland socket");
            return false;
        }
        if (!wlr_backend_start(backend)) {
            wlr_log(WLR_ERROR, "failed to start backend");
            return false;
        }

        setenv("WAYLAND_DISPLAY", socket, true);
        wlr_log(WLR_INFO, "fenriz running on WAYLAND_DISPLAY=%s", socket);

        // Screen sharing: xdg-desktop-portal picks its backend by XDG_CURRENT_DESKTOP.
        // Identify as fenriz so the fenriz-portals.conf routes ScreenCast to the
        // wlr backend, which captures via our wlr-screencopy global.
        setenv("XDG_CURRENT_DESKTOP", "fenriz:wlroots", 1);
        // The portal runs as a systemd/D-Bus user service and reads its own activation
        // env, not ours, push the vars it needs.
        spawn("dbus-update-activation-environment --systemd XDG_CURRENT_DESKTOP WAYLAND_DISPLAY");

        // Control socket (FENRIZ_SOCKET) — needs WAYLAND_DISPLAY set, and must be up before
        // exec_once so bars/tools spawned below inherit the env and can connect immediately.
        ipc::init(*this);

        // Hot-reload: apply edits to fenriz.conf live (no restart). See init_config_watch.
        init_config_watch(*this, loop);

        // Run startup commands now that the socket is live and WAYLAND_DISPLAY is set,
        // so the spawned clients connect to us.
        for (const std::string& cmd : config.exec_once)
            spawn(cmd);

        // DBG: exercise workspace switching without a physical keyboard. Cycles
        // ws0 -> ws1 (empty) -> ws0 on a timer so a headless run can verify filtering/focus.
        if (getenv("FENRIZ_DBG_WORKSPACES")) {
            static wl_event_source* t = wl_event_loop_add_timer(
                loop,
                [](void* data) -> int {
                    Server* s = static_cast<Server*>(data);
                    output::Output* o = output::focused(*s);
                    int next = (o && o->active_ws == 0) ? 1 : 0;
                    wlr_log(WLR_INFO, "fenriz DBG: timer switching to ws %d", next);
                    set_workspace(*s, next);
                    return 0;
                },
                this);
            wl_event_source_timer_update(t, 2500);
        }

        return true;
    }

    void reload_config(Server& server) {
        server.config = Config::load(); // built-in defaults if the file was removed
        // Re-apply output mode/scale/position and workspace homes, then re-home + re-arrange.
        // Editing an `output =` line takes effect live, no restart.
        output::apply_config(server);
        cursor::reload(server); // `cursor =` / `cursor_size =` re-theme the pointer live
        for (View* v : server.views)
            place_view_nodes(v); // border width/color/rounding on all views (incl. floating)
        wlr_log(WLR_INFO, "fenriz: config reloaded");
    }

    void Server::run() { wl_display_run(display); }

    void Server::stop() {
        if (display)
            wl_display_terminate(display);
    }

} // namespace fenriz
