#include "server.hpp"

#include "log.hpp"
#include <wlr/util/log.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    fenriz::log::init();

    fenriz::Server server;
    if (!server.start()) {
        wlr_log(WLR_ERROR, "fenriz: failed to start");
        return 1;
    }
    server.run();
    return 0;
}
