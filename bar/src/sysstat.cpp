#include "sysstat.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <sstream>

namespace fenriz::bar {

    namespace {

        constexpr guint INTERVAL_MS = 1500;

        std::string read_file(const std::string& path) {
            char* text = nullptr;
            gsize len = 0;
            if (!g_file_get_contents(path.c_str(), &text, &len, nullptr))
                return "";
            std::string out(text, len);
            g_free(text);
            return out;
        }

        std::string trimmed(std::string s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
                s.pop_back();
            return s;
        }

        // The CPU package sensor: a known hwmon driver's first input, else the first thermal zone.
        std::string find_temp_path() {
            constexpr const char* HWMON = "/sys/class/hwmon";
            if (GDir* dir = g_dir_open(HWMON, 0, nullptr)) {
                std::string found;
                while (const char* entry = g_dir_read_name(dir)) {
                    const std::string base = std::string(HWMON) + "/" + entry;
                    const std::string name = trimmed(read_file(base + "/name"));
                    if (name == "k10temp" || name == "zenpower" || name == "coretemp" || name == "cpu_thermal") {
                        found = base + "/temp1_input";
                        break;
                    }
                }
                g_dir_close(dir);
                if (!found.empty())
                    return found;
            }
            const std::string zone = "/sys/class/thermal/thermal_zone0/temp";
            return g_file_test(zone.c_str(), G_FILE_TEST_EXISTS) ? zone : "";
        }

    } // namespace

    CpuTimes parse_cpu(const std::string& proc_stat) {
        std::istringstream in(proc_stat);
        std::string label;
        in >> label;
        CpuTimes t;
        if (label != "cpu")
            return t;
        // user nice system idle iowait irq softirq steal; guest time is already inside user/nice
        uint64_t fields[8] = {};
        for (auto& f : fields)
            in >> f;
        for (uint64_t f : fields)
            t.total += f;
        t.idle = fields[3] + fields[4];
        return t;
    }

    int cpu_percent(const CpuTimes& before, const CpuTimes& after) {
        if (after.total <= before.total)
            return 0;
        const uint64_t total = after.total - before.total;
        const uint64_t idle = after.idle >= before.idle ? after.idle - before.idle : 0;
        return static_cast<int>(100 * (total - std::min(idle, total)) / total);
    }

    int parse_memory_percent(const std::string& meminfo) {
        std::istringstream in(meminfo);
        std::string key;
        uint64_t value = 0, total = 0, available = 0;
        std::string unit;
        while (in >> key >> value) {
            if (key == "MemTotal:")
                total = value;
            else if (key == "MemAvailable:")
                available = value;
            std::getline(in, unit);
        }
        if (total == 0 || available > total)
            return -1;
        return static_cast<int>(100 * (total - available) / total);
    }

    SysStat::SysStat() : temp_path_(find_temp_path()) {}

    SysStat::~SysStat() {
        if (tick_id_)
            g_source_remove(tick_id_);
    }

    void SysStat::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void SysStat::watch(bool on) {
        watchers_ = std::max(0, watchers_ + (on ? 1 : -1));
        if (watchers_ > 0 && !tick_id_) {
            last_cpu_ = parse_cpu(read_file("/proc/stat"));
            tick_id_ = g_timeout_add(INTERVAL_MS, on_tick, this);
            sample();
        } else if (watchers_ == 0 && tick_id_) {
            g_source_remove(tick_id_);
            tick_id_ = 0;
        }
    }

    gboolean SysStat::on_tick(gpointer data) {
        static_cast<SysStat*>(data)->sample();
        return G_SOURCE_CONTINUE;
    }

    void SysStat::sample() {
        const CpuTimes now = parse_cpu(read_file("/proc/stat"));
        // the first reading after watch() has nothing to compare against yet, so it keeps the last known value
        if (now.total > last_cpu_.total)
            latest_.cpu = cpu_percent(last_cpu_, now);
        last_cpu_ = now;
        latest_.memory = parse_memory_percent(read_file("/proc/meminfo"));

        struct statvfs fs;
        if (statvfs("/", &fs) == 0 && fs.f_blocks > 0)
            latest_.disk = static_cast<int>(100 * (fs.f_blocks - fs.f_bfree) / fs.f_blocks);

        if (!temp_path_.empty()) {
            const std::string text = read_file(temp_path_);
            latest_.celsius = text.empty() ? -1 : g_ascii_strtod(text.c_str(), nullptr) / 1000.0;
        }

        if (latest_.cpu >= 0) {
            cpu_history_.push_back(latest_.cpu);
            memory_history_.push_back(std::max(latest_.memory, 0));
            if (cpu_history_.size() > HISTORY) {
                cpu_history_.pop_front();
                memory_history_.pop_front();
            }
        }
        for (auto& listener : listeners_)
            listener();
    }

} // namespace fenriz::bar
