#include "spawn.hpp"

#include <fcntl.h>
#include <gio/gdesktopappinfo.h>
#include <unistd.h>

namespace fenriz::desktop::spawn {

    namespace {

        // Runs in the forked child between fork and exec
        void detach_child(gpointer) {
            setsid();
            const int null = open("/dev/null", O_RDWR);
            if (null < 0)
                return;
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }

    } // namespace

    bool app(GAppInfo* info) {
        GError* err = nullptr;
        gboolean ok;
        if (G_IS_DESKTOP_APP_INFO(info)) {
            // launch_uris_as_manager is the only entry point that accepts a child setup.
            ok = g_desktop_app_info_launch_uris_as_manager(G_DESKTOP_APP_INFO(info),
                                                           nullptr,
                                                           nullptr,
                                                           G_SPAWN_SEARCH_PATH,
                                                           detach_child,
                                                           nullptr,
                                                           nullptr,
                                                           nullptr,
                                                           &err);
        } else {
            ok = g_app_info_launch(info, nullptr, nullptr, &err);
        }
        if (!ok) {
            g_warning("spawn: %s: %s", g_app_info_get_display_name(info), err->message);
            g_error_free(err);
        }
        return ok;
    }

    bool command(const std::string& cmd) {
        if (cmd.empty())
            return false;
        char* argv[] = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>(cmd.c_str()), nullptr};
        GError* err = nullptr;
        if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, detach_child, nullptr, nullptr, &err)) {
            g_warning("spawn: %s: %s", cmd.c_str(), err->message);
            g_error_free(err);
            return false;
        }
        return true;
    }

} // namespace fenriz::desktop::spawn
