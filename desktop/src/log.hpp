#pragma once

#include <string>

namespace fenriz::desktop::log {

    // $FENRIZ_DESKTOP_LOG, else $XDG_STATE_HOME/fenriz/fenriz-desktop.log. Another `name`
    // (fenriz-bar) reads $FENRIZ_BAR_LOG and writes fenriz-bar.log.
    std::string path(const std::string& name = "fenriz-desktop");

    // Sends g_message/g_warning to that file as well as stderr
    void init(const std::string& name = "fenriz-desktop");

} // namespace fenriz::desktop::log
