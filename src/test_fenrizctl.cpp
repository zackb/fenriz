#include <cassert>
#include <cstdio>

#include "fenrizctl.hpp"

using namespace fenrizctl;

namespace {

    Command run(std::initializer_list<const char*> argv) {
        return parse(std::vector<std::string>(argv.begin(), argv.end()));
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
    assert(run({"workspace", "11"}).mode == Mode::None);
    assert(run({"workspace"}).mode == Mode::None);
    assert(run({"dpms", "maybe"}).mode == Mode::None);
    assert(run({"output", "eDP-1"}).mode == Mode::None);
    assert(run({"lid", "shut"}).mode == Mode::None);
    assert(run({"exec"}).mode == Mode::None); // exec with no command
    assert(run({"movetoworkspace"}).mode == Mode::None);
    assert(run({"killactiv"}).mode == Mode::None); // typo, not silently ignored

    printf("fenrizctl tests passed\n");
    return 0;
}
