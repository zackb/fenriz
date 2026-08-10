#include "frecency.hpp"

#include <glib.h>

namespace fenriz::desktop {

    namespace {
        constexpr int64_t DAY = 86400000; // ms
    } // namespace

    double recency_weight(int64_t age_ms) {
        if (age_ms < DAY)
            return 1.0;
        if (age_ms < 3 * DAY)
            return 0.7;
        if (age_ms < 7 * DAY)
            return 0.5;
        if (age_ms < 30 * DAY)
            return 0.25;
        return 0.1;
    }

    double frecency(int count, int64_t last_used_ms, int64_t now_ms) {
        if (count <= 0)
            return 0.0;
        const int64_t age = now_ms > last_used_ms ? now_ms - last_used_ms : 0;
        return count * recency_weight(age);
    }

    int64_t now_ms() { return g_get_real_time() / 1000; }

    std::string UsageStore::path() {
        const char* state = g_getenv("XDG_STATE_HOME");
        std::string dir = (state && *state) ? std::string(state) : std::string(g_get_home_dir()) + "/.local/state";
        return dir + "/fenriz/launcher.usage";
    }

    void UsageStore::load() {
        uses_.clear();
        GKeyFile* kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path().c_str(), G_KEY_FILE_NONE, nullptr)) {
            gsize n = 0;
            char** groups = g_key_file_get_groups(kf, &n);
            for (gsize i = 0; i < n; i++) {
                Use u;
                u.count = g_key_file_get_integer(kf, groups[i], "count", nullptr);
                u.last = g_key_file_get_int64(kf, groups[i], "last", nullptr);
                if (u.count > 0)
                    uses_[groups[i]] = u;
            }
            g_strfreev(groups);
        }
        g_key_file_free(kf);
    }

    void UsageStore::record(const std::string& id, int64_t now) {
        if (id.empty())
            return;
        Use& u = uses_[id];
        u.count += 1;
        u.last = now;
        save();
    }

    double UsageStore::score_of(const std::string& id, int64_t now) const {
        auto it = uses_.find(id);
        if (it == uses_.end())
            return 0.0;
        return frecency(it->second.count, it->second.last, now);
    }

    void UsageStore::save() const {
        const std::string file = path();
        char* dir = g_path_get_dirname(file.c_str());
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);

        GKeyFile* kf = g_key_file_new();
        for (const auto& [id, u] : uses_) {
            g_key_file_set_integer(kf, id.c_str(), "count", u.count);
            g_key_file_set_int64(kf, id.c_str(), "last", u.last);
        }
        GError* err = nullptr;
        if (!g_key_file_save_to_file(kf, file.c_str(), &err)) {
            g_warning("launcher: cannot save usage: %s", err->message);
            g_error_free(err);
        }
        g_key_file_free(kf);
    }

} // namespace fenriz::desktop
