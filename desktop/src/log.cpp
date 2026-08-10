#include "log.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <ctime>

namespace fenriz::desktop::log {

    namespace {

        // Second destination for every log line stderr keeps getting them too.
        FILE* logfile = nullptr;

        const char* level_name(GLogLevelFlags level) {
            switch (level & G_LOG_LEVEL_MASK) {
            case G_LOG_LEVEL_ERROR:
                return "error";
            case G_LOG_LEVEL_CRITICAL:
                return "critical";
            case G_LOG_LEVEL_WARNING:
                return "warning";
            case G_LOG_LEVEL_DEBUG:
                return "debug";
            case G_LOG_LEVEL_INFO:
                return "info";
            default:
                return "message";
            }
        }

        GLogWriterOutput writer(GLogLevelFlags level, const GLogField* fields, gsize n_fields, gpointer) {
            const GLogLevelFlags bare = static_cast<GLogLevelFlags>(level & G_LOG_LEVEL_MASK);
            // GTK and the Vulkan loader are chatty at info/debug.
            if ((bare == G_LOG_LEVEL_DEBUG || bare == G_LOG_LEVEL_INFO) && !g_getenv("FENRIZ_DEBUG"))
                return G_LOG_WRITER_HANDLED;

            const char* message = "";
            const char* domain = nullptr;
            for (gsize i = 0; i < n_fields; i++) {
                if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
                    message = static_cast<const char*>(fields[i].value);
                else if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
                    domain = static_cast<const char*>(fields[i].value);
            }

            timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            tm parts;
            localtime_r(&ts.tv_sec, &parts);
            char stamp[16];
            strftime(stamp, sizeof stamp, "%H:%M:%S", &parts);

            const char* prefix = (domain && g_strcmp0(domain, "fenriz-desktop") != 0) ? domain : "";
            const char* sep = *prefix ? ": " : "";
            for (FILE* out : {stderr, logfile})
                if (out)
                    fprintf(out,
                            "%s.%03ld [%s] %s%s%s\n",
                            stamp,
                            ts.tv_nsec / 1000000,
                            level_name(level),
                            prefix,
                            sep,
                            message);
            return G_LOG_WRITER_HANDLED;
        }

    } // namespace

    std::string path() {
        if (const char* p = g_getenv("FENRIZ_DESKTOP_LOG"); p && *p)
            return p;
        const char* state = g_getenv("XDG_STATE_HOME");
        if (state && *state)
            return std::string(state) + "/fenriz/fenriz-desktop.log";
        return std::string(g_get_home_dir()) + "/.local/state/fenriz/fenriz-desktop.log";
    }

    void init() {
        const std::string p = path();
        char* dir = g_path_get_dirname(p.c_str());
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);

        // Keep one generation, same as the compositor's log.
        const std::string previous = p + ".1";
        g_rename(p.c_str(), previous.c_str());

        logfile = fopen(p.c_str(), "we");
        if (logfile)
            setvbuf(logfile, nullptr, _IOLBF, 0);

        g_log_set_writer_func(writer, nullptr, nullptr);
        g_message("logging to %s", p.c_str());
    }

} // namespace fenriz::desktop::log
