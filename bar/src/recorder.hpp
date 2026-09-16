#pragma once

#include <gio/gio.h>

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    // The file name for a recording started at `when`, in the local timezone.
    std::string recording_filename(std::time_t when);

    // wf-recorder's argv for one recording. `output` is a connector name, `audio` a PipeWire node name.
    std::vector<std::string>
        recorder_argv(const std::string& output, const std::string& audio, const std::string& path);

    // One screen recording, encoded by wf-recorder over wlr-screencopy.
    class Recorder {
    public:
        Recorder() = default;
        ~Recorder();

        Recorder(const Recorder&) = delete;
        Recorder& operator=(const Recorder&) = delete;

        // False when wf-recorder is not installed.
        static bool available();

        bool start(const std::string& output, const std::string& audio);
        void stop();

        bool recording() const { return process_ != nullptr; }
        int elapsed_seconds() const;
        const std::string& path() const { return path_; }

        // The finished recording's path, and whether wf-recorder exited cleanly.
        void on_finished(std::function<void(const std::string&, bool)> listener) { finished_ = std::move(listener); }
        void on_changed(std::function<void()> listener) { changed_ = std::move(listener); }

    private:
        void read_next();
        void reap(bool ok);

        static void on_line(GObject* source, GAsyncResult* result, gpointer data);
        static void on_wait(GObject* source, GAsyncResult* result, gpointer data);

        std::string path_;
        std::string last_error_; // last stderr line, only when wf-recorder fails
        std::function<void(const std::string&, bool)> finished_;
        std::function<void()> changed_;
        GCancellable* cancellable_ = nullptr;
        GSubprocess* process_ = nullptr;
        GDataInputStream* stderr_ = nullptr;
        gint64 started_us_ = 0;
    };

} // namespace fenriz::bar
