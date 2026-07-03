#include "server.hpp"

#include "wlr.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    wlr_log_init(WLR_INFO, nullptr);

    fenriz::Server server;
    if (!server.start()) {
        wlr_log(WLR_ERROR, "fenriz: failed to start");
        return 1;
    }
    server.run();
    return 0;
}
