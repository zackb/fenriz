#include "ipc.hpp"

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "lock.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::ipc {

    namespace {

        struct Client {
            int fd;
            wl_event_source* src; // its readable source in the loop; removed on disconnect
        };

        // One instance per compositor. Set in init(); publish()/callbacks reach it here.
        struct IpcState {
            Server* server = nullptr;
            wl_event_loop* loop = nullptr;
            int listen_fd = -1;
            std::vector<Client> clients;
            std::string last; // last broadcast snapshot; skip re-sending if unchanged
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
            s += "]},\"activeWindow\":";
            View* f = server.focused_view;
            if (f && f->mapped) {
                s += "{\"appId\":\"";
                json_escape(s, view_app_id(f)); // xdg app_id or X11 WM_CLASS
                s += "\",\"title\":\"";
                json_escape(s, view_title(f));
                s += "\"}";
            } else {
                s += "null";
            }
            s += "}\n";
            return s;
        }

        void drop_client(int fd) {
            auto& c = g->clients;
            for (auto it = c.begin(); it != c.end(); ++it) {
                if (it->fd == fd) {
                    wl_event_source_remove(it->src);
                    close(fd);
                    c.erase(it);
                    return;
                }
            }
        }

        // Send one line; drop the client if the pipe is broken.
        void send_line(int fd, const std::string& line) {
            if (send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0)
                drop_client(fd);
        }

        // Pull a "key":"value" string out of a command line. Same substring approach as the
        // rest of the parser — no escape handling, which is fine for output names.
        std::string extract_string(const std::string& line, const char* key) {
            const std::string pat = std::string("\"") + key + "\":\"";
            size_t p = line.find(pat);
            if (p == std::string::npos)
                return "";
            p += pat.size();
            size_t e = line.find('"', p);
            return e == std::string::npos ? "" : line.substr(p, e - p);
        }

        // ponytail: parses whole "\n"-terminated lines in one read; a command split across
        // reads is dropped. Commands are single tiny writes (socat/printf), so fine — add
        // per-client line buffering only if a real client streams partial commands.
        void handle_command_line(Server& server, const std::string& line) {
            if (line.find("\"cmd\":\"workspace\"") != std::string::npos) {
                size_t p = line.find("\"n\":");
                if (p == std::string::npos)
                    return;
                int n = std::atoi(line.c_str() + p + 4);
                if (n >= 1 && n <= 10)
                    set_workspace(server, n - 1);
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

        int on_client_readable(int fd, uint32_t mask, void* data) {
            (void)mask;
            auto* st = static_cast<IpcState*>(data);
            char buf[4096];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { // EOF or error -> client gone
                drop_client(fd);
                return 0;
            }
            std::string chunk(buf, n);
            size_t start = 0, nl;
            while ((nl = chunk.find('\n', start)) != std::string::npos) {
                handle_command_line(*st->server, chunk.substr(start, nl - start));
                start = nl + 1;
            }
            return 0;
        }

        int on_listen_readable(int fd, uint32_t mask, void* data) {
            (void)mask;
            auto* st = static_cast<IpcState*>(data);
            int cfd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd < 0)
                return 0;
            wl_event_source* src = wl_event_loop_add_fd(st->loop, cfd, WL_EVENT_READABLE, on_client_readable, st);
            st->clients.push_back({cfd, src});
            send_line(cfd, snapshot(*st->server)); // greet with current state
            return 0;
        }

    } // namespace

    void init(Server& server) {
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const char* disp = getenv("WAYLAND_DISPLAY");
        if (!xdg || !disp) {
            wlr_log(WLR_ERROR, "fenriz ipc: XDG_RUNTIME_DIR/WAYLAND_DISPLAY unset, no control socket");
            return;
        }
        std::string path = std::string(xdg) + "/fenriz-" + disp + ".sock";

        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            wlr_log(WLR_ERROR, "fenriz ipc: socket() failed");
            return;
        }
        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            wlr_log(WLR_ERROR, "fenriz ipc: socket path too long: %s", path.c_str());
            close(fd);
            return;
        }
        std::strcpy(addr.sun_path, path.c_str());
        unlink(path.c_str()); // clear a stale socket from a prior run
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
            wlr_log(WLR_ERROR, "fenriz ipc: bind/listen failed on %s", path.c_str());
            close(fd);
            return;
        }

        g = new IpcState{};
        g->server = &server;
        g->loop = wl_display_get_event_loop(server.display);
        g->listen_fd = fd;
        wl_event_loop_add_fd(g->loop, fd, WL_EVENT_READABLE, on_listen_readable, g);

        setenv("FENRIZ_SOCKET", path.c_str(), true);
        wlr_log(WLR_INFO, "fenriz ipc: listening on FENRIZ_SOCKET=%s", path.c_str());
    }

    void publish(Server& server) {
        if (!g)
            return;
        std::string s = snapshot(server);
        if (s == g->last)
            return;
        g->last = s;
        // Snapshot the fds first: send_line may drop clients (EPIPE) and mutate g->clients.
        std::vector<int> fds;
        for (const Client& c : g->clients)
            fds.push_back(c.fd);
        for (int fd : fds)
            send_line(fd, s);
    }

} // namespace fenriz::ipc
