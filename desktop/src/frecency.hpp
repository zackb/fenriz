#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace fenriz::desktop {

    // Recency multiplier applied to a launch count.
    double recency_weight(int64_t age_ms);

    double frecency(int count, int64_t last_used_ms, int64_t now_ms);

    // Launch counts, persisted to $XDG_STATE_HOME/fenriz-desktop/launcher.usage.
    class UsageStore {
    public:
        struct Use {
            int count = 0;
            int64_t last = 0; // ms since the epoch
        };

        void load();
        void record(const std::string& id, int64_t now_ms);
        double score_of(const std::string& id, int64_t now_ms) const;

        static std::string path();

        const std::map<std::string, Use>& uses() const { return uses_; }
        void seed(std::string id, Use use) { uses_[std::move(id)] = use; }

    private:
        void save() const;

        std::map<std::string, Use> uses_;
    };

    int64_t now_ms();

} // namespace fenriz::desktop
