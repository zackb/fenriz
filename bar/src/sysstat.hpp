#pragma once

#include <glib.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    struct CpuTimes {
        uint64_t total = 0;
        uint64_t idle = 0; // idle + iowait
    };

    // The aggregate "cpu" line of /proc/stat.
    CpuTimes parse_cpu(const std::string& proc_stat);

    // Busy share between two readings, 0..100.
    int cpu_percent(const CpuTimes& before, const CpuTimes& after);

    // Used share of memory from /proc/meminfo (MemTotal - MemAvailable), 0..100; -1 when unreadable.
    int parse_memory_percent(const std::string& meminfo);

    // CPU, memory, disk and temperature, sampled only while someone is looking.
    class SysStat {
    public:
        static constexpr size_t HISTORY = 40;

        struct Sample {
            int cpu = -1;
            int memory = -1;
            int disk = -1;       // used share of /
            double celsius = -1; // CPU package, -1 without a sensor
        };

        SysStat();
        ~SysStat();

        SysStat(const SysStat&) = delete;
        SysStat& operator=(const SysStat&) = delete;

        // Sampling runs while at least one watcher is active; each call to watch(true) needs a watch(false).
        void watch(bool on);
        void subscribe(std::function<void()> listener);

        const Sample& latest() const { return latest_; }
        const std::deque<int>& cpu_history() const { return cpu_history_; }
        const std::deque<int>& memory_history() const { return memory_history_; }

    private:
        void sample();
        static gboolean on_tick(gpointer data);

        std::string temp_path_;
        CpuTimes last_cpu_;
        Sample latest_;
        std::deque<int> cpu_history_;
        std::deque<int> memory_history_;
        int watchers_ = 0;
        guint tick_id_ = 0;
        std::vector<std::function<void()>> listeners_;
    };

} // namespace fenriz::bar
