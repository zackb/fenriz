#include "flip.hpp"

#include <algorithm>
#include <vector>

#include "ipc.hpp"
#include "output.hpp"
#include "server.hpp"
#include "tiling.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::flip {

    namespace {

        // The visible half of `v`'s pair, or null when `v` isn't paired.
        View* front_of(View* v) {
            if (!v || !v->flip_peer)
                return nullptr;
            return v->flip_back ? v->flip_peer : v;
        }

        void join(View* front, View* back) {
            front->flip_peer = back;
            back->flip_peer = front;
            front->flip_back = false;
            back->flip_back = true;
            back->flip_t = 1.0;
        }

        // Break the pair `v` belongs to, leaving both halves ordinary windows again.
        void split(View* v) {
            View* peer = v->flip_peer;
            v->flip_peer = nullptr;
            v->flip_back = false;
            v->flip_t = 1.0;
            if (peer) {
                peer->flip_peer = nullptr;
                peer->flip_back = false;
                peer->flip_t = 1.0;
            }
        }

        // Trade the halves' roles.
        void swap_roles(Server& server, View* front) {
            View* back = front->flip_peer;
            if (!back)
                return;
            if (!front->floating)
                if (tiling::Node* leaf = tiling::find_leaf(server.workspaces[front->workspace].root, front))
                    leaf->view = back;
            back->box = front->box;
            back->flip_back = false;
            front->flip_back = true;
            back->flip_t = front->flip_t;
            front->flip_t = 1.0;
            view_flip_release(front); // it is hidden now give buffer back
            Workspace& ws = server.workspaces[front->workspace];
            if (ws.last_focused == front)
                ws.last_focused = back;
            view_configure(back);
            const bool had_focus = server.focused_view == front;
            place_view_nodes(front);
            place_view_nodes(back);
            if (back->flip_t < 1.0)
                view_flip_capture(back);
            if (had_focus)
                focus_view(server, back);
            ipc::publish(server);
        }

        void schedule(Server& server, View* v) {
            if (output::Output* o = view_output(server, v))
                wlr_output_schedule_frame(o->handle);
        }

    } // namespace

    void mark(Server& server) {
        View* v = server.focused_view;
        if (!v || v->flip_peer)
            return;
        server.flip_mark = v;
    }

    void pair(Server& server) {
        View* front = server.flip_mark;
        View* back = server.focused_view;
        // Both halves must be free to share one tile: unpaired, same workspace, same
        // tiled/floating state, neither fullscreen.
        if (!front || !back || front == back || front->flip_peer || back->flip_peer)
            return;
        if (front->workspace != back->workspace || front->floating != back->floating)
            return;
        if (front->fullscreen || back->fullscreen)
            return;
        if (!back->floating)
            tiling::remove(server, back); // the front's tile is now the pair's tile
        join(front, back);
        server.flip_mark = nullptr;
        Workspace& ws = server.workspaces[front->workspace];
        if (ws.last_focused == back)
            ws.last_focused = front;
        tiling::arrange(server);
        focus_view(server, front);
        ipc::publish(server);
    }

    void turn(Server& server) {
        View* front = front_of(server.focused_view);
        if (!front)
            return;
        if (server.config.flip_ms <= 0) {
            swap_roles(server, front);
            tiling::arrange(server);
            return;
        }
        if (front->flip_t >= 1.0) {  // starting a turn
            place_view_nodes(front); // re-run the content clip
            view_flip_capture(front);
        }
        // Re-pressing mid-turn mirrors the progress: the squash is symmetric about the pinch,
        // so the frame width is unchanged and the turn runs back the way it came.
        front->flip_t = front->flip_t >= 1.0 ? 0.0 : 1.0 - front->flip_t;
        schedule(server, front);
    }

    void unpair(Server& server) {
        View* front = front_of(server.focused_view);
        if (!front)
            return;
        View* back = front->flip_peer;
        const bool floating = front->floating;
        view_flip_release(front);
        view_flip_release(back);
        split(front);
        if (floating) {
            // Two floats sharing one box would land exactly on top of each other.
            back->box.x += 32;
            back->box.y += 32;
            view_configure(back);
        } else {
            tiling::insert(server, back, front); // bisects the tile they were sharing
        }
        tiling::arrange(server);
        ipc::publish(server);
    }

    void forget(Server& server, View* view) {
        if (server.flip_mark == view)
            server.flip_mark = nullptr;
        View* peer = view->flip_peer;
        if (!peer)
            return;
        const bool was_front = !view->flip_back;
        view_flip_release(view);
        view_flip_release(peer);
        split(view);
        // The survivor keeps the tile: a dying front hands its leaf over rather than letting
        // tree_remove collapse it and strand the back with no slot at all.
        if (was_front && !peer->floating)
            if (tiling::Node* leaf = tiling::find_leaf(server.workspaces[view->workspace].root, view))
                leaf->view = peer;
        peer->box = view->box;
        view_configure(peer);
    }

    bool tick(Server& server, output::Output* out, double dt) {
        const double step = dt / (std::max(1, server.config.flip_ms) / 1000.0);
        bool animating = false;
        std::vector<View*> pinched;
        for (View* v : server.views) {
            if (v->flip_t >= 1.0 || v->flip_back)
                continue;
            if (!view_visible(server, v) || view_output(server, v) != out)
                continue;
            const double prev = v->flip_t;
            v->flip_t = std::min(1.0, v->flip_t + step);
            if (v->flip_t < 1.0)
                animating = true;
            if (past_pinch(prev, v->flip_t)) {
                pinched.push_back(v); // swapping mutates the view list; do it outside the walk
                continue;
            }
            if (v->flip_t >= 1.0)
                view_flip_release(v);
            place_view_nodes(v);
        }
        for (View* v : pinched)
            swap_roles(server, v);
        return animating;
    }

} // namespace fenriz::flip
