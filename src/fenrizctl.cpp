// fenrizctl: command-line client for the fenriz control socket (see docs/IPC.md).
//
// Connect, optionally read, optionally write one JSON line, exit.

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "fenrizctl.hpp"

namespace {

    const char* USAGE = "usage: fenrizctl COMMAND [ARGS]\n"
                        "\n"
                        "  state                 print the current state as one JSON line\n"
                        "  watch                 stream state changes as newline-delimited JSON\n"
                        "\n"
                        "  workspace N           show workspace N (1-32)\n"
                        "  dpms on|off [OUTPUT]  power displays on/off (all, or one connector)\n"
                        "  output NAME on|off    enable/disable an output\n"
                        "  lid open|closed       set the lid state and run the clamshell policy\n"
                        "  reload                re-read fenriz.conf\n"
                        "  unlock                force-unlock a stuck session lock\n"
                        "  exit                  quit the compositor\n"
                        "\n"
                        "  ACTION [ARG]          run a keybind action: killactive, fullscreen,\n"
                        "                        togglefloating, pin, focusnext/prev/left/right/up/down,\n"
                        "                        movetoworkspace N, exec CMD\n"
                        "\n"
                        "Reads $FENRIZ_SOCKET, falling back to\n"
                        "$XDG_RUNTIME_DIR/fenriz-$WAYLAND_DISPLAY.sock, and from a TTY (no\n"
                        "WAYLAND_DISPLAY) to the only $XDG_RUNTIME_DIR/fenriz-*.sock present.\n"
                        "\n"
                        "  fenrizctl state | jq .windows      # the window list\n"
                        "  fenrizctl movetoworkspace 3\n";

    int connect_socket(const std::string& path) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;
        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            close(fd);
            return -1;
        }
        std::strcpy(addr.sun_path, path.c_str());
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    // Read until `buf` holds a complete line and return it (including newline) or "" on EOF
    std::string read_line(int fd, std::string& buf) {
        for (;;) {
            if (size_t nl = buf.find('\n'); nl != std::string::npos) {
                std::string line = buf.substr(0, nl + 1);
                buf.erase(0, nl + 1);
                return line;
            }
            char chunk[8192];
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return "";
            buf.append(chunk, n);
        }
    }

    bool write_all(int fd, const std::string& s) {
        for (size_t off = 0; off < s.size();) {
            ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            off += (size_t)n;
        }
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (!args.empty() && (args[0] == "help" || args[0] == "-h" || args[0] == "--help")) {
        fputs(USAGE, stdout);
        return 0;
    }
    if (!args.empty() && (args[0] == "--version" || args[0] == "-v")) {
        printf("fenrizctl %s\n", FENRIZ_VERSION);
        return 0;
    }

    fenrizctl::Command cmd = fenrizctl::parse(args);
    if (cmd.mode == fenrizctl::Mode::None) {
        fprintf(stderr, "fenrizctl: %s\n\n%s", cmd.error.c_str(), USAGE);
        return 1;
    }

    std::string path = fenrizctl::socket_path();
    if (path.empty()) {
        fprintf(stderr,
                "fenrizctl: no socket. Set FENRIZ_SOCKET, or WAYLAND_DISPLAY, e.g.\n"
                "  WAYLAND_DISPLAY=wayland-0 fenrizctl %s\n",
                args[0].c_str());
        return 2;
    }
    int fd = connect_socket(path);
    if (fd < 0) {
        fprintf(stderr, "fenrizctl: cannot connect to %s — is fenriz running?\n", path.c_str());
        return 2;
    }

    // Every connection gets the current state, so there is always one line to
    // read before anything else happens.
    std::string buf;
    std::string snapshot = read_line(fd, buf);
    if (snapshot.empty()) {
        fprintf(stderr, "fenrizctl: connection closed before any state arrived\n");
        return 2;
    }

    if (cmd.mode == fenrizctl::Mode::State) {
        fputs(snapshot.c_str(), stdout);
        return 0;
    }
    if (cmd.mode == fenrizctl::Mode::Watch) {
        fputs(snapshot.c_str(), stdout);
        fflush(stdout);
        for (std::string line; !(line = read_line(fd, buf)).empty();) {
            fputs(line.c_str(), stdout);
            fflush(stdout); // line-buffer for `fenrizctl watch | while read`
        }
        return 0;
    }

    if (!cmd.output.empty() && snapshot.find("\"name\":\"" + cmd.output + "\"") == std::string::npos) {
        fprintf(stderr, "fenrizctl: no enabled output named '%s'\n", cmd.output.c_str());
        return 1;
    }
    if (!write_all(fd, cmd.json)) {
        fprintf(stderr, "fenrizctl: failed to send command\n");
        return 2;
    }
    shutdown(fd, SHUT_WR);
    return 0;
}
