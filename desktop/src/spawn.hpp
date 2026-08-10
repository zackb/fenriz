#pragma once

#include <gio/gio.h>

#include <string>

namespace fenriz::desktop::spawn {

    // Launch a desktop entry.
    bool app(GAppInfo* info);

    // Run a shell command
    bool command(const std::string& cmd);

} // namespace fenriz::desktop::spawn
