#include "recorder.hpp"

#include <glib/gstdio.h>

#include <csignal>

namespace fenriz::bar {

    namespace {

        constexpr const char* PROGRAM = "wf-recorder";

        constexpr const char* EXTENSION = ".mp4";

        // Created on demand. XDG_VIDEOS_DIR.
        std::string videos_dir() {
            const char* dir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
            if (!dir)
                dir = g_get_home_dir();
            g_mkdir_with_parents(dir, 0700);
            return dir;
        }

    } // namespace

    std::string recording_filename(std::time_t when) {
        std::tm tm = {};
        localtime_r(&when, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        return std::string("fenriz-recording-") + stamp + EXTENSION;
    }

    std::vector<std::string>
        recorder_argv(const std::string& output, const std::string& audio, const std::string& path) {
        std::vector<std::string> argv = {
            PROGRAM,
            "-y",
            "-f",
            path,
            "-F",
            "scale=in_range=full:out_range=full:in_color_matrix=bt709:out_color_matrix=bt709",
            "-p",
            "colorspace=bt709"};
        if (!output.empty()) {
            argv.push_back("-o");
            argv.push_back(output);
        }
        if (!audio.empty())
            argv.push_back("-a" + audio); // wf-recorder wants the device glued to the flag
        return argv;
    }

    bool Recorder::available() {
        char* found = g_find_program_in_path(PROGRAM);
        const bool ok = found != nullptr;
        g_free(found);
        return ok;
    }

    Recorder::~Recorder() {
        if (!process_)
            return;

        g_cancellable_cancel(cancellable_);
        g_subprocess_send_signal(process_, SIGINT);
        g_subprocess_wait(process_, nullptr, nullptr);
        g_clear_object(&stderr_);
        g_clear_object(&process_);
        g_clear_object(&cancellable_);
    }

    bool Recorder::start(const std::string& output, const std::string& audio) {
        if (process_)
            return false;

        const std::string path = videos_dir() + "/" + recording_filename(std::time(nullptr));
        const std::vector<std::string> args = recorder_argv(output, audio, path);
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);

        GSubprocessLauncher* launcher = g_subprocess_launcher_new(
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
        GError* err = nullptr;
        GSubprocess* process = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
        g_object_unref(launcher);
        if (!process) {
            g_warning("recorder: %s", err->message);
            g_error_free(err);
            return false;
        }

        path_ = path;
        process_ = process;
        last_error_.clear();
        started_us_ = g_get_monotonic_time();
        cancellable_ = g_cancellable_new();
        stderr_ = g_data_input_stream_new(g_subprocess_get_stderr_pipe(process_));
        read_next();
        g_subprocess_wait_async(process_, cancellable_, on_wait, this);
        if (changed_)
            changed_();
        return true;
    }

    void Recorder::stop() {
        if (process_)
            g_subprocess_send_signal(process_, SIGINT);
    }

    int Recorder::elapsed_seconds() const {
        if (!process_)
            return 0;
        return static_cast<int>((g_get_monotonic_time() - started_us_) / G_USEC_PER_SEC);
    }

    void Recorder::read_next() {
        g_data_input_stream_read_line_async(stderr_, G_PRIORITY_DEFAULT, cancellable_, on_line, this);
    }

    void Recorder::on_line(GObject* source, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<Recorder*>(data);
        gsize length = 0;
        char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, nullptr);
        if (!line)
            return; // EOF or cancelled

        if (length) {
            g_debug("recorder: %s", line);
            self->last_error_ = line;
        }
        g_free(line);
        self->read_next();
    }

    void Recorder::on_wait(GObject* source, GAsyncResult* result, gpointer data) {
        GError* err = nullptr;
        const bool waited = g_subprocess_wait_finish(G_SUBPROCESS(source), result, &err);
        if (!waited && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(err);
            return;
        }
        if (err)
            g_error_free(err);

        // sigint is how the recording is stopped
        auto* self = static_cast<Recorder*>(data);
        self->reap(g_subprocess_get_if_signaled(self->process_) || g_subprocess_get_exit_status(self->process_) == 0);
    }

    void Recorder::reap(bool ok) {
        if (!ok)
            g_warning("recorder: %s", last_error_.empty() ? "wf-recorder failed" : last_error_.c_str());
        const std::string path = path_;
        g_cancellable_cancel(cancellable_);
        g_clear_object(&stderr_);
        g_clear_object(&process_);
        g_clear_object(&cancellable_);
        path_.clear();
        if (changed_)
            changed_();
        if (finished_)
            finished_(path, ok);
    }

} // namespace fenriz::bar
