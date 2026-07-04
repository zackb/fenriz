#include "ipc.hpp"

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

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
            std::set<int> occupied;
            occupied.insert(server.active_workspace + 1); // active is always shown
            for (View* v : server.views)
                if (v->mapped)
                    occupied.insert(v->workspace + 1);

            std::string s = "{\"workspaces\":{\"active\":";
            s += std::to_string(server.active_workspace + 1);
            s += ",\"occupied\":[";
            bool first = true;
            for (int id : occupied) {
                if (!first)
                    s += ',';
                first = false;
                s += std::to_string(id);
            }
            s += "]},\"activeWindow\":";
            View* f = server.focused_view;
            if (f && f->mapped) {
                s += "{\"appId\":\"";
                json_escape(s, f->toplevel->app_id);
                s += "\",\"title\":\"";
                json_escape(s, f->toplevel->title);
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

        // ponytail: parses whole "\n"-terminated lines in one read; a command split across
        // reads is dropped. Commands are single tiny writes (socat/printf), so fine — add
        // per-client line buffering only if a real client streams partial commands.
        void handle_command_line(Server& server, const std::string& line) {
            if (line.find("\"cmd\":\"workspace\"") == std::string::npos)
                return;
            size_t p = line.find("\"n\":");
            if (p == std::string::npos)
                return;
            int n = std::atoi(line.c_str() + p + 4);
            if (n >= 1 && n <= 10)
                set_workspace(server, n - 1);
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
            wl_event_source* src =
                wl_event_loop_add_fd(st->loop, cfd, WL_EVENT_READABLE, on_client_readable, st);
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
