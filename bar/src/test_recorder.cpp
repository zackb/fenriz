#include <cassert>
#include <ctime>

#include "recorder.hpp"

using fenriz::bar::recorder_argv;
using fenriz::bar::recording_filename;

namespace {

    std::time_t local_time(int year, int month, int day, int hour, int minute, int second) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        tm.tm_isdst = -1;
        return std::mktime(&tm);
    }

} // namespace

int main() {
    assert(recording_filename(local_time(2026, 9, 15, 7, 4, 9)) == "fenriz-recording-20260915-070409.mkv");
    assert(recording_filename(local_time(2026, 12, 31, 23, 59, 59)) == "fenriz-recording-20261231-235959.mkv");

    // no audio
    const std::vector<std::string> silent = {"wf-recorder", "-y", "-f", "/v/a.mkv", "-o", "DP-1"};
    assert(recorder_argv("DP-1", "", "/v/a.mkv") == silent);

    // a microphone, glued to the flag as wf-recorder wants it
    const std::vector<std::string> mic = {
        "wf-recorder", "-y", "-f", "/v/a.mkv", "-o", "DP-1", "-aalsa_input.pci-0000_00_1f.3.analog-stereo"};
    assert(recorder_argv("DP-1", "alsa_input.pci-0000_00_1f.3.analog-stereo", "/v/a.mkv") == mic);

    // system audio is a sink monitor
    assert(recorder_argv("DP-1", "alsa_output.x.monitor", "/v/a.mkv").back() == "-aalsa_output.x.monitor");

    // no focused output: wf-recorder picks one itself rather than being passed an empty name
    const std::vector<std::string> any = {"wf-recorder", "-y", "-f", "/v/a.mkv"};
    assert(recorder_argv("", "", "/v/a.mkv") == any);

    // a path with a space survives, since nothing goes through a shell
    assert(recorder_argv("DP-1", "", "/v/my videos/a.mkv")[3] == "/v/my videos/a.mkv");
}
