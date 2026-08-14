#include "ipc.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "keyboard.hpp"
#include "lock.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"
#include "workspace_protocol.hpp"

namespace fenriz::ipc {

    namespace {

        struct Client {
            int fd;
            wl_event_source* src; // its readable source in the loop; removed on disconnect
        };

        // One listening socket and the clients attached to it.
        struct Feed {
            wl_event_source* listen_src = nullptr;
            std::string path;
            std::vector<Client> clients;
        };

        // One instance per compositor. Set in init(); publish()/callbacks reach it here.
        struct IpcState {
            Server* server = nullptr;
            wl_event_loop* loop = nullptr;
            Feed state;
            Feed events;
            std::string last;             // last broadcast snapshot; skip re-sending if unchanged
            bool publish_pending = false; // an idle broadcast is already queued this iteration
        };
        IpcState* g = nullptr;

        void json_escape(std::string& out, const char* s) {
            if (!s)
                return;
            for (; *s; ++s) {
                unsigned char c = *s;
                if (c == '"' || c == '\\')
                    out += '\\', out += (char)c;
                else if (c == '\n')
                    out += "\\n";
                else if (c < 0x20)
                    ; // drop other control chars
                else
                    out += (char)c;
            }
        }

        std::string snapshot(Server& server) {
            output::Output* focus = output::focused(server);

            std::set<int> occupied;
            for (View* v : server.views)
                if (v->mapped)
                    occupied.insert(v->workspace + 1);
            // Whatever each screen is showing counts as occupied even if it's empty, so a bar
            // always renders the workspace you're looking at.
            for (output::Output* o : server.outputs)
                if (o->enabled && o->active_ws >= 0)
                    occupied.insert(o->active_ws + 1);

            std::set<int> urgent;
            for (View* v : server.views)
                if (v->mapped && v->urgent)
                    urgent.insert(v->workspace + 1);

            // Per-output state. This is what lets a shell rebuild itself on hotplug instead of
            // being reloaded: the outputs come and go in this feed as their globals do.
            std::string s = "{\"outputs\":[";
            bool first_o = true;
            for (output::Output* o : server.outputs) {
                if (!o->enabled)
                    continue; // a disabled panel has no wl_output either; don't advertise it
                wlr_box box;
                wlr_output_layout_get_box(server.output_layout, o->handle, &box);
                if (!first_o)
                    s += ',';
                first_o = false;
                s += "{\"name\":\"";
                json_escape(s, output::name_of(o).c_str());
                s += "\",\"active\":" + std::to_string(o->active_ws + 1);
                s += ",\"focused\":" + std::string(o == focus ? "true" : "false");
                s += ",\"x\":" + std::to_string(box.x);
                s += ",\"y\":" + std::to_string(box.y);
                s += ",\"width\":" + std::to_string(box.width);
                s += ",\"height\":" + std::to_string(box.height);
                // The output's ACTUAL committed scale, not the configured one. Reporting the
                // config here made the feed agree with fenriz.conf while the panel really ran
                // at 1x, which hid a scale bug instead of surfacing it. Report what is, not
                // what was asked for.
                s += ",\"scale\":" + std::to_string(o->handle->scale);
                s += ",\"internal\":" + std::string(output::is_internal(output::name_of(o)) ? "true" : "false");
                s += '}';
            }
            s += "],\"lid\":\"";
            s += server.lid_closed ? "closed" : "open";

            // Cursor in layout coordinates, so it composes with outputs[].x/y above.
            s += "\",\"cursor\":{\"x\":";
            s += std::to_string((int)server.cursor->x);
            s += ",\"y\":" + std::to_string((int)server.cursor->y);

            // workspaces.active stays the focused output's workspace, so existing single-screen
            // bars keep working unchanged; `outputs` above carries the per-screen detail.
            s += "},\"workspaces\":{\"active\":";
            s += std::to_string(focus && focus->active_ws >= 0 ? focus->active_ws + 1 : 0);
            s += ",\"occupied\":[";
            bool first = true;
            for (int id : occupied) {
                if (!first)
                    s += ',';
                first = false;
                s += std::to_string(id);
            }
            s += "],\"urgent\":[";
            first = true;
            for (int id : urgent) {
                if (!first)
                    s += ',';
                first = false;
                s += std::to_string(id);
            }
            s += "]},\"windows\":[";
            first = true;
            for (View* v : server.views) {
                if (!v->mapped)
                    continue;
                if (!first)
                    s += ',';
                first = false;
                s += "{\"appId\":\"";
                json_escape(s, view_app_id(v));
                s += "\",\"title\":\"";
                json_escape(s, view_title(v));
                s += "\",\"icon\":\"";
                json_escape(s, view_icon(v));
                s += "\",\"tag\":\"";
                json_escape(s, view_tag(v));
                s += "\",\"workspace\":" + std::to_string(v->workspace + 1);
                s += ",\"floating\":" + std::string(v->floating ? "true" : "false");
                s += ",\"fullscreen\":" + std::string(v->fullscreen ? "true" : "false");
                s += ",\"focused\":" + std::string(v == server.focused_view ? "true" : "false");
                s += ",\"urgent\":" + std::string(v->urgent ? "true" : "false");
                s += '}';
            }

            s += "],\"activeWindow\":";
            View* f = server.focused_view;
            if (f && f->mapped) {
                s += "{\"appId\":\"";
                json_escape(s, view_app_id(f)); // xdg app_id or X11 WM_CLASS
                s += "\",\"title\":\"";
                json_escape(s, view_title(f));
                s += "\",\"icon\":\"";
                json_escape(s, view_icon(f));
                s += "\",\"tag\":\"";
                json_escape(s, view_tag(f));
                s += "\"}";
            } else {
                s += "null";
            }
            s += "}\n";
            return s;
        }

        void drop_client(Feed& feed, int fd) {
            auto& c = feed.clients;
            for (auto it = c.begin(); it != c.end(); ++it) {
                if (it->fd == fd) {
                    // wl_event_source_remove closes the fd it owns
                    wl_event_source_remove(it->src);
                    c.erase(it);
                    return;
                }
            }
        }

        // Send one line, looping partial writes. False means the client is unusable and the
        // caller should drop it
        bool send_line(int fd, const std::string& line) {
            size_t off = 0;
            while (off < line.size()) {
                ssize_t n = send(fd, line.data() + off, line.size() - off, MSG_NOSIGNAL);
                if (n > 0) {
                    off += (size_t)n;
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return off == 0;
                return false; // EPIPE/ECONNRESET or n == 0: client is gone
            }
            return true;
        }

        void broadcast(Feed& feed, const std::string& line) {
            // snapshot the fds first: send_line may drop clients (EPIPE) and mutate the vector.
            std::vector<int> fds;
            for (const Client& c : feed.clients)
                fds.push_back(c.fd);
            for (int fd : fds)
                if (!send_line(fd, line))
                    drop_client(feed, fd);
        }

        // Pull a "key":"value" string out of a command line, undoing \" and \\ escapes. Same
        // substring approach as the rest of the parser; the escapes matter because a dispatch
        // `exec` arg is an arbitrary shell command that may quote.
        std::string extract_string(const std::string& line, const char* key) {
            const std::string pat = std::string("\"") + key + "\":\"";
            size_t p = line.find(pat);
            if (p == std::string::npos)
                return "";
            std::string out;
            for (p += pat.size(); p < line.size(); ++p) {
                if (line[p] == '"')
                    return out;
                if (line[p] == '\\' && p + 1 < line.size())
                    ++p;
                out += line[p];
            }
            return "";
        }

        void handle_command_line(Server& server, const std::string& line) {
            if (line.find("\"cmd\":\"workspace\"") != std::string::npos) {
                size_t p = line.find("\"n\":");
                if (p == std::string::npos)
                    return;
                int n = std::atoi(line.c_str() + p + 4);
                if (n >= 1 && n <= server.config.workspaces)
                    set_workspace(server, n - 1);
                return;
            }
            if (line.find("\"cmd\":\"dispatch\"") != std::string::npos) {
                Bind b;
                b.action = action_from_string(extract_string(line, "action"));
                b.arg = extract_string(line, "arg");
                if (b.action != Action::None)
                    execute_bind(server, b);
                return;
            }
            if (line.find("\"cmd\":\"reload\"") != std::string::npos) {
                reload_config(server);
                return;
            }
            if (line.find("\"cmd\":\"unlock\"") != std::string::npos) {
                lock::force_unlock(server);
                return;
            }
            if (line.find("\"cmd\":\"exit\"") != std::string::npos) {
                // {"cmd":"exit"} — quit the compositor, same as the `exit` keybind action.
                // Under a session manager (greetd) that ends the session, i.e. log out.
                server.stop();
                return;
            }
            if (line.find("\"cmd\":\"dpms\"") != std::string::npos) {
                // {"cmd":"dpms","on":true} powers on; anything else (e.g. "on":false) powers
                // off. An optional "name" targets one screen; without it, all of them.
                output::Output* o = nullptr;
                if (std::string n = extract_string(line, "name"); !n.empty())
                    o = output::by_name(server, n);
                output::set_dpms(server, o, line.find("\"on\":true") != std::string::npos);
                return;
            }
            if (line.find("\"cmd\":\"output\"") != std::string::npos) {
                // {"cmd":"output","name":"eDP-1","enabled":false} — enable/disable a screen.
                std::string n = extract_string(line, "name");
                if (n.empty())
                    return;
                if (output::Output* o = output::by_name(server, n)) {
                    output::set_enabled(server, o, line.find("\"enabled\":true") != std::string::npos);
                    // refresh, not apply_layout: disabling the external with the lid shut has
                    // to bring the panel back, which is the lid policy's call.
                    output::refresh(server);
                }
                return;
            }
            if (line.find("\"cmd\":\"lid\"") != std::string::npos) {
                // {"cmd":"lid","closed":true} — drives the same policy a real lid switch does,
                // so clamshell behavior is testable in a nested session with no hardware.
                server.lid_closed = line.find("\"closed\":true") != std::string::npos;
                output::refresh(server);
                return;
            }
        }

        // Read one chunk from a client, or drop it. Returns the bytes read, or -1 when the
        // client is gone (already dropped) or the wakeup was spurious.
        ssize_t read_or_drop(Feed& feed, int fd, char* buf, size_t cap) {
            ssize_t n = recv(fd, buf, cap, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return -1; // spurious wakeup, not a disconnect
            if (n <= 0) {  // error, or orderly EOF -> client gone
                drop_client(feed, fd);
                return -1;
            }
            return n;
        }

        int on_state_readable(int fd, uint32_t mask, void* data) {
            (void)mask;
            auto* feed = static_cast<Feed*>(data);
            char buf[4096];
            ssize_t n = read_or_drop(*feed, fd, buf, sizeof(buf));
            if (n < 0)
                return 0;
            std::string chunk(buf, n);
            size_t start = 0, nl;
            while ((nl = chunk.find('\n', start)) != std::string::npos) {
                handle_command_line(*g->server, chunk.substr(start, nl - start));
                start = nl + 1;
            }
            return 0;
        }

        // The event feed takes no commands; this exists only so EOF drops the client.
        int on_events_readable(int fd, uint32_t mask, void* data) {
            (void)mask;
            char buf[4096];
            read_or_drop(*static_cast<Feed*>(data), fd, buf, sizeof(buf));
            return 0;
        }

        int accept_client(Feed& feed, int fd, wl_event_loop_fd_func_t on_readable) {
            int cfd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd < 0)
                return -1;
            wl_event_source* src = wl_event_loop_add_fd(g->loop, cfd, WL_EVENT_READABLE, on_readable, &feed);
            feed.clients.push_back({cfd, src});
            return cfd;
        }

        int on_state_listen(int fd, uint32_t mask, void* data) {
            (void)mask;
            auto* feed = static_cast<Feed*>(data);
            if (int cfd = accept_client(*feed, fd, on_state_readable); cfd >= 0)
                send_line(cfd, snapshot(*g->server)); // greet with the current state
            return 0;
        }

        // No greeting: the event feed has no backlog, only what happens from now on.
        int on_events_listen(int fd, uint32_t mask, void* data) {
            (void)mask;
            accept_client(*static_cast<Feed*>(data), fd, on_events_readable);
            return 0;
        }

    } // namespace

    namespace {
        // Bind and listen on a Unix stream socket, replacing any stale file left by a prior
        // run. Returns the fd, or -1 with the reason already logged.
        int bind_socket(const std::string& path) {
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (fd < 0) {
                wlr_log(WLR_ERROR, "fenriz ipc: socket() failed");
                return -1;
            }
            sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            if (path.size() >= sizeof(addr.sun_path)) {
                wlr_log(WLR_ERROR, "fenriz ipc: socket path too long: %s", path.c_str());
                close(fd);
                return -1;
            }
            std::strcpy(addr.sun_path, path.c_str());
            unlink(path.c_str());
            if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
                wlr_log(WLR_ERROR, "fenriz ipc: bind/listen failed on %s", path.c_str());
                close(fd);
                return -1;
            }
            return fd;
        }
    } // namespace

    void init(Server& server) {
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const char* disp = getenv("WAYLAND_DISPLAY");
        if (!xdg || !disp) {
            wlr_log(WLR_ERROR, "fenriz ipc: XDG_RUNTIME_DIR/WAYLAND_DISPLAY unset, no control socket");
            return;
        }
        // The event socket deliberately does NOT end in .sock: fenrizctl falls back to globbing
        // fenriz-*.sock from a TTY and refuses when that matches more than one file.
        std::string path = std::string(xdg) + "/fenriz-" + disp + ".sock";
        std::string event_path = std::string(xdg) + "/fenriz-" + disp + ".events";

        int fd = bind_socket(path);
        if (fd < 0)
            return;

        g = new IpcState{};
        g->server = &server;
        g->loop = wl_display_get_event_loop(server.display);
        g->state.path = path;
        g->state.listen_src = wl_event_loop_add_fd(g->loop, fd, WL_EVENT_READABLE, on_state_listen, &g->state);
        if (!g->state.listen_src) {
            close(fd);
            delete g;
            g = nullptr;
            wlr_log(WLR_ERROR, "fenriz ipc: could not register the listen socket");
            return;
        }

        setenv("FENRIZ_SOCKET", path.c_str(), true);
        wlr_log(WLR_INFO, "fenriz ipc: listening on FENRIZ_SOCKET=%s", path.c_str());

        // The event feed is a bonus channel: if it can't be brought up, the compositor and
        // every existing command still work, so warn and carry on.
        int efd = bind_socket(event_path);
        if (efd < 0) {
            wlr_log(WLR_ERROR, "fenriz ipc: no event socket; bells and other events won't be published");
            return;
        }
        g->events.path = event_path;
        g->events.listen_src = wl_event_loop_add_fd(g->loop, efd, WL_EVENT_READABLE, on_events_listen, &g->events);
        if (!g->events.listen_src) {
            close(efd);
            g->events.path.clear();
            wlr_log(WLR_ERROR, "fenriz ipc: could not register the event socket");
            return;
        }

        setenv("FENRIZ_EVENT_SOCKET", event_path.c_str(), true);
        wlr_log(WLR_INFO, "fenriz ipc: listening on FENRIZ_EVENT_SOCKET=%s", event_path.c_str());
    }

    namespace {
        // The actual broadcast, run once per event-loop iteration from the idle source
        // scheduled by publish(). Coalescing here means a burst of publish() calls (e.g. a
        // focused window rewriting its title many times a second) builds the snapshot once,
        // not once per call.
        void do_publish() {
            g->publish_pending = false;
            workspace_protocol::sync(*g->server);
            if (g->state.clients.empty()) // nobody listening: don't build the snapshot at all
                return;
            std::string s = snapshot(*g->server);
            if (s == g->last)
                return;
            g->last = s;
            broadcast(g->state, s);
        }

        void publish_idle(void* data) {
            (void)data;
            do_publish();
        }
    } // namespace

    void publish(Server& server) {
        (void)server;
        if (!g || g->publish_pending)
            return;
        g->publish_pending = true;
        wl_event_loop_add_idle(g->loop, publish_idle, nullptr);
    }

    void bell(Server& server, wlr_surface* surface) {
        if (!g || g->events.clients.empty())
            return;

        // Naming a surface is optional, and the one named may belong to no view at all (a layer
        // surface, or a window already gone). Anything unattributable rings without window keys.
        View* rang = nullptr;
        if (surface)
            for (View* v : server.views)
                if (view_surface(v) == surface) {
                    rang = v;
                    break;
                }

        std::string s = "{\"event\":\"bell\"";
        if (rang) {
            s += ",\"appId\":\"";
            json_escape(s, view_app_id(rang));
            s += "\",\"title\":\"";
            json_escape(s, view_title(rang));
            s += "\",\"workspace\":" + std::to_string(rang->workspace + 1);
        }
        s += "}\n";
        broadcast(g->events, s);
    }

    void shutdown() {
        if (!g)
            return;
        for (Feed* f : {&g->state, &g->events}) {
            for (const Client& c : f->clients)
                wl_event_source_remove(c.src);
            if (f->listen_src)
                wl_event_source_remove(f->listen_src);
            if (!f->path.empty())
                unlink(f->path.c_str());
        }
        delete g;
        g = nullptr;
    }

} // namespace fenriz::ipc
