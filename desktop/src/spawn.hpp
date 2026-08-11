#pragma once

#include <gio/gio.h>

#include <string>

namespace fenriz::desktop::spawn {

    // Launch a desktop entry.
    bool app(GAppInfo* info);

    // Run a shell command.
    bool command(const std::string& cmd);

    // Run a shell command with `arg` available as $1.
    bool hook(const std::string& cmd, const std::string& arg);

} // namespace fenriz::desktop::spawn
