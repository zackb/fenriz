#pragma once

#include <gio/gio.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fenriz::bar {

    // What a plugin last reported, in the island's own vocabulary. The bar draws it.
    struct PluginPill { // a passing notice in the collapsed pill
        std::string text;
        std::string icon;  // icon name
        std::string image; // file path, preferred over icon
        bool operator==(const PluginPill&) const = default;
    };

    struct PluginChip { // footer text on Home
        std::string text;
        std::string icon;
        bool operator==(const PluginChip&) const = default;
    };

    struct PluginTile { // a Home tile
        std::string title;
        std::string icon;
        std::string image;
        bool operator==(const PluginTile&) const = default;
    };

    // One block of a page. `type` picks which fields matter:
    // row (image/icon, text, subtitle, trailing, action), text (text, style), table (columns, rows, highlight),
    // list (items: rows), level (value 0..1), button (text, action).
    struct PluginBlock {
        std::string type;
        std::string text;
        std::string subtitle;
        std::string trailing;
        std::string icon;
        std::string image;
        std::string style; // text: dim, section, title
        std::string action;
        double value = 0;
        int highlight = -1; // table row index
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        std::vector<PluginBlock> items;
        bool operator==(const PluginBlock&) const = default;
    };

    struct PluginPage {
        std::string title;
        std::vector<PluginBlock> blocks;
        bool operator==(const PluginPage&) const = default;
    };

    struct PluginState {
        std::optional<PluginPill> pill;
        std::optional<PluginChip> chip;
        std::optional<PluginTile> tile;
        std::optional<PluginPage> page;
    };

    enum PluginSlot : unsigned {
        SLOT_PILL = 1,
        SLOT_CHIP = 2,
        SLOT_TILE = 4,
        SLOT_PAGE = 8,
    };

    // Applies one line of plugin output: each slot present replaces that slot, null clears it. Returns the slots
    // that actually changed; false in `valid` for a line that is not a JSON object.
    unsigned apply_plugin_line(PluginState& state, const std::string& line, bool* valid = nullptr);

    // {"event": "open"}, or {"event": "action", "id": "..."} when `id` is given.
    std::string plugin_event(const char* event, const std::string& id = {});

    // A running plugin: `sh -c COMMAND` with NDJSON state on stdout, events on stdin and stderr in the log. Restarted
    // with backoff when it exits, its state cleared meanwhile.
    class Plugin {
    public:
        Plugin(std::string name, std::string command);
        ~Plugin();

        Plugin(const Plugin&) = delete;
        Plugin& operator=(const Plugin&) = delete;

        void start();
        // Dropped if the plugin is gone or not reading.
        void send(const std::string& line);

        const std::string& name() const { return name_; }
        const PluginState& state() const { return state_; }
        void on_change(std::function<void(unsigned slots)> listener) { listener_ = std::move(listener); }

    private:
        void spawn();
        void read_next(GDataInputStream* stream);
        void exited();

        static void on_line(GObject* source, GAsyncResult* result, gpointer data);
        static void on_wait(GObject* source, GAsyncResult* result, gpointer data);
        static gboolean on_restart(gpointer data);

        std::string name_;
        std::string command_;
        PluginState state_;
        std::function<void(unsigned)> listener_;
        GCancellable* cancellable_ = nullptr;
        GSubprocess* process_ = nullptr;
        GDataInputStream* stdout_ = nullptr;
        GDataInputStream* stderr_ = nullptr;
        int stdin_fd_ = -1;
        gint64 started_us_ = 0;
        guint backoff_s_ = 1;
        guint restart_id_ = 0;
    };

} // namespace fenriz::bar
