#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "fenrizctl.hpp"

using namespace fenrizctl;

namespace {

    Command run(std::initializer_list<const char*> argv) {
        return parse(std::vector<std::string>(argv.begin(), argv.end()));
    }

    void touch(const std::string& path) {
        FILE* f = fopen(path.c_str(), "w");
        assert(f);
        fclose(f);
    }

    // The TTY case: no WAYLAND_DISPLAY, so the socket has to be discovered.
    void test_socket_path() {
        char dir[] = "/tmp/fenrizctl-test-XXXXXX";
        assert(mkdtemp(dir));
        const std::string one = std::string(dir) + "/fenriz-wayland-0.sock";

        // FENRIZ_SOCKET wins over everything, even a display that would resolve.
        assert(socket_path("/run/explicit.sock", dir, "wayland-0") == "/run/explicit.sock");
        assert(socket_path("", dir, "wayland-1") == std::string(dir) + "/fenriz-wayland-1.sock");

        // Nothing to discover yet.
        assert(socket_path(nullptr, dir, nullptr).empty());
        assert(socket_path(nullptr, nullptr, nullptr).empty());

        touch(one);
        assert(socket_path(nullptr, dir, nullptr) == one);
        assert(socket_path(nullptr, dir, "") == one); // empty display counts as unset

        // The event socket lives in the same directory. It must not end in .sock, or the
        // discovery above would see two candidates and give up.
        const std::string ev = std::string(dir) + "/fenriz-wayland-0.events";
        touch(ev);
        assert(socket_path(nullptr, dir, nullptr) == one);

        // Two compositors running: guessing would talk to the wrong session.
        const std::string two = std::string(dir) + "/fenriz-wayland-1.sock";
        touch(two);
        assert(socket_path(nullptr, dir, nullptr).empty());

        unlink(one.c_str());
        unlink(two.c_str());
        unlink(ev.c_str());
        rmdir(dir);
    }

    void test_event_socket_path() {
        // FENRIZ_EVENT_SOCKET wins, even over a state path that would resolve.
        assert(event_socket_path("/run/explicit.events", "/run/fenriz-wayland-0.sock") == "/run/explicit.events");

        // Otherwise derived from the state path by swapping the suffix.
        assert(event_socket_path(nullptr, "/run/fenriz-wayland-0.sock") == "/run/fenriz-wayland-0.events");
        assert(event_socket_path("", "/run/fenriz-wayland-0.sock") == "/run/fenriz-wayland-0.events");

        // FENRIZ_SOCKET can point anywhere; a path with no .sock suffix just gains .events.
        assert(event_socket_path(nullptr, "/run/weird") == "/run/weird.events");
        assert(event_socket_path(nullptr, ".sock") == ".sock.events"); // suffix, not whole name

        // No state socket to derive from and no override: nothing to connect to.
        assert(event_socket_path(nullptr, "").empty());
    }

    // The sent line, without its trailing newline.
    std::string sent(std::initializer_list<const char*> argv) {
        Command c = parse(std::vector<std::string>(argv.begin(), argv.end()));
        assert(c.mode == Mode::Send);
        assert(!c.json.empty() && c.json.back() == '\n');
        return c.json.substr(0, c.json.size() - 1);
    }

} // namespace

int main() {
    // Read commands don't send anything.
    assert(run({"state"}).mode == Mode::State);
    assert(run({"watch"}).mode == Mode::Watch);
    assert(run({"events"}).mode == Mode::Events);

    // Every command documented in docs/IPC.md.
    assert(sent({"workspace", "3"}) == R"({"cmd":"workspace","n":3})");
    assert(sent({"dpms", "off"}) == R"({"cmd":"dpms","on":false})");
    assert(sent({"dpms", "on", "DP-1"}) == R"({"cmd":"dpms","on":true,"name":"DP-1"})");
    assert(sent({"output", "eDP-1", "off"}) == R"({"cmd":"output","name":"eDP-1","enabled":false})");
    assert(sent({"lid", "closed"}) == R"({"cmd":"lid","closed":true})");
    assert(sent({"lid", "open"}) == R"({"cmd":"lid","closed":false})");
    assert(sent({"unlock"}) == R"({"cmd":"unlock"})");
    assert(sent({"reload"}) == R"({"cmd":"reload"})");
    assert(sent({"exit"}) == R"({"cmd":"exit"})");

    // dispatch, with and without the `dispatch` word.
    assert(sent({"killactive"}) == R"({"cmd":"dispatch","action":"killactive"})");
    assert(sent({"dispatch", "killactive"}) == sent({"killactive"}));
    assert(sent({"movetoworkspace", "3"}) == R"({"cmd":"dispatch","action":"movetoworkspace","arg":"3"})");

    assert(sent({"exec", "foot", "-e", "sh"}) == R"({"cmd":"dispatch","action":"exec","arg":"foot -e sh"})");
    assert(sent({"exec", "sh -c \"echo hi\""}) == R"({"cmd":"dispatch","action":"exec","arg":"sh -c \"echo hi\""})");

    // A disabled output isn't in the feed, so only a disable is name-checked.
    assert(run({"output", "eDP-1", "off"}).output == "eDP-1");
    assert(run({"output", "eDP-1", "on"}).output.empty());
    assert(run({"dpms", "off", "DP-1"}).output == "DP-1");
    assert(run({"dpms", "off"}).output.empty());

    // Usage errors, caught before anything touches the socket.
    assert(run({}).mode == Mode::None);
    assert(run({"workspace", "0"}).mode == Mode::None);
    assert(run({"workspace", "33"}).mode == Mode::None); // past the cap
    assert(run({"workspace"}).mode == Mode::None);
    assert(run({"dpms", "maybe"}).mode == Mode::None);
    assert(run({"output", "eDP-1"}).mode == Mode::None);
    assert(run({"lid", "shut"}).mode == Mode::None);
    assert(run({"exec"}).mode == Mode::None); // exec with no command
    assert(run({"movetoworkspace"}).mode == Mode::None);
    assert(run({"killactiv"}).mode == Mode::None); // typo, not silently ignored

    test_socket_path();
    test_event_socket_path();

    printf("fenrizctl tests passed\n");
    return 0;
}
