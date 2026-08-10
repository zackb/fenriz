#pragma once

#include <string>

namespace fenriz::desktop::log {

    // $FENRIZ_DESKTOP_LOG, else $XDG_STATE_HOME/fenriz/fenriz-desktop.log.
    std::string path();

    // Sends g_message/g_warning to that file as well as stderr
    void init();

} // namespace fenriz::desktop::log
