#pragma once

#include <gio/gio.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fenriz::bar {

    // The slice of fenriz's IPC state feed (docs/IPC.md) the bar draws.
    struct CompositorState {
        struct Output {
            std::string name;
            int active = 0; // workspace shown, 1-indexed
            bool focused = false;

            bool operator==(const Output&) const = default;
        };

        std::vector<Output> outputs;
        std::vector<int> occupied;
        std::vector<int> urgent;
        bool has_window = false; // activeWindow was not null
        std::string title;
        std::string app_id;
        std::string icon;

        // The workspace `output` shows, or 0 when there is no such output.
        int active_on(const std::string& output) const;
        // Connector name of the focused output, or "".
        std::string focused_output() const;

        bool operator==(const CompositorState&) const = default;
    };

    // One NDJSON line of the state feed. Empty on anything that is not a state object.
    std::optional<CompositorState> parse_state(const std::string& line);

    // Follows $FENRIZ_SOCKET.
    // TODO: move to ext-workspace-v1 + foreign-toplevel once fenriz reports a workspace group per output; with one
    // group spanning every output a per-screen bar cannot tell which workspace each screen shows.
    class Compositor {
    public:
        using Listener = std::function<void(const CompositorState&)>;

        Compositor() = default;
        ~Compositor();

        Compositor(const Compositor&) = delete;
        Compositor& operator=(const Compositor&) = delete;

        // Connects and keeps reconnecting. The listener sees an empty state while disconnected.
        void start(Listener listener);

        void workspace(int n);
        void exit(); // quits the compositor, which ends the session
        bool connected() const { return conn_ != nullptr; }

        const CompositorState& state() const { return state_; }

    private:
        void connect();
        void disconnect();
        void read_next();
        void publish(CompositorState state);
        void send(const std::string& line);

        static void on_connected(GObject* source, GAsyncResult* res, gpointer data);
        static void on_line(GObject* source, GAsyncResult* res, gpointer data);
        static gboolean on_retry(gpointer data);

        std::string path_;
        Listener listener_;
        CompositorState state_;
        GCancellable* cancel_ = nullptr;
        GSocketConnection* conn_ = nullptr;
        GDataInputStream* in_ = nullptr;
        guint retry_id_ = 0;
    };

} // namespace fenriz::bar
