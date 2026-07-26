#include "log.hpp"

#include "wlr.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <wayland-server-core.h>

namespace fenriz::log {

    namespace {

        // Second destination for every log line
        FILE* logfile = nullptr;

        wlr_log_importance log_level = WLR_INFO;

        void log_write(const char* tag, const char* fmt, va_list args) {
            timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            tm parts;
            localtime_r(&ts.tv_sec, &parts);
            char stamp[16];
            strftime(stamp, sizeof stamp, "%H:%M:%S", &parts);
            // libwayland's messages already end in a newline, wlroots' don't.
            const size_t len = strlen(fmt);
            const char* end = (len && fmt[len - 1] == '\n') ? "" : "\n";

            va_list copy;
            va_copy(copy, args);
            fprintf(stderr, "%s.%03ld [%s] ", stamp, ts.tv_nsec / 1000000, tag);
            vfprintf(stderr, fmt, args);
            fputs(end, stderr);
            if (logfile) {
                fprintf(logfile, "%s.%03ld [%s] ", stamp, ts.tv_nsec / 1000000, tag);
                vfprintf(logfile, fmt, copy);
                fputs(end, logfile);
            }
            va_end(copy);
        }

        void wlr_handler(wlr_log_importance level, const char* fmt, va_list args) {
            if (level > log_level)
                return;
            static const char* names[] = {"silent", "error", "info", "debug"};
            log_write(level < WLR_LOG_IMPORTANCE_LAST ? names[level] : "info", fmt, args);
        }

        void wayland_handler(const char* fmt, va_list args) { log_write("wayland", fmt, args); }

        void open_file() {
            const std::string p = path();
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
            // keep one generation
            rename(p.c_str(), (p + ".1").c_str());
            logfile = fopen(p.c_str(), "we");
            if (logfile)
                setvbuf(logfile, nullptr, _IOLBF, 0);
        }

    } // namespace

    std::string path() {
        if (const char* p = getenv("FENRIZ_LOG"))
            return p;
        const char* state = getenv("XDG_STATE_HOME");
        if (state && *state)
            return std::string(state) + "/fenriz/fenriz.log";
        const char* home = getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.local/state/fenriz/fenriz.log";
    }

    void init() {
        open_file();
        log_level = getenv("FENRIZ_DEBUG") ? WLR_DEBUG : WLR_INFO;
        wlr_log_init(log_level, wlr_handler);
        wl_log_set_handler_server(wayland_handler);
        wlr_log(WLR_INFO, "fenriz: logging to %s", path().c_str());
    }

} // namespace fenriz::log
