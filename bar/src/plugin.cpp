#include "plugin.hpp"

#include <fcntl.h>
#include <gio/gunixoutputstream.h>
#include <json-glib/json-glib.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>

namespace fenriz::bar {

    namespace {

        constexpr guint MAX_BACKOFF_S = 60;
        constexpr gint64 HEALTHY_US = 30 * G_USEC_PER_SEC; // a run this long resets the backoff

        // Scalars as text, so a plugin may send a score as 3 or "3".
        std::string text_of(JsonNode* node) {
            if (!node || !JSON_NODE_HOLDS_VALUE(node))
                return {};
            switch (json_node_get_value_type(node)) {
            case G_TYPE_STRING:
                return json_node_get_string(node);
            case G_TYPE_INT64:
                return std::to_string(json_node_get_int(node));
            case G_TYPE_DOUBLE: {
                char buf[G_ASCII_DTOSTR_BUF_SIZE];
                return g_ascii_formatd(buf, sizeof buf, "%g", json_node_get_double(node));
            }
            case G_TYPE_BOOLEAN:
                return json_node_get_boolean(node) ? "true" : "false";
            default:
                return {};
            }
        }

        std::string str(JsonObject* o, const char* key) { return text_of(json_object_get_member(o, key)); }

        double num(JsonObject* o, const char* key, double fallback) {
            JsonNode* n = json_object_get_member(o, key);
            if (!n || !JSON_NODE_HOLDS_VALUE(n))
                return fallback;
            GType t = json_node_get_value_type(n);
            return t == G_TYPE_INT64 || t == G_TYPE_DOUBLE ? json_node_get_double(n) : fallback;
        }

        JsonArray* array(JsonObject* o, const char* key) {
            JsonNode* n = json_object_get_member(o, key);
            return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : nullptr;
        }

        std::vector<std::string> strings(JsonArray* a) {
            std::vector<std::string> out;
            for (guint i = 0; a && i < json_array_get_length(a); i++)
                out.push_back(text_of(json_array_get_element(a, i)));
            return out;
        }

        PluginBlock parse_block(JsonObject* o) {
            PluginBlock b;
            b.type = str(o, "type");
            b.text = str(o, "text");
            b.subtitle = str(o, "subtitle");
            b.trailing = str(o, "trailing");
            b.icon = str(o, "icon");
            b.image = str(o, "image");
            b.style = str(o, "style");
            b.action = str(o, "action");
            b.value = std::clamp(num(o, "value", 0), 0.0, 1.0);
            b.highlight = static_cast<int>(num(o, "highlight", -1));
            b.columns = strings(array(o, "columns"));
            if (JsonArray* rows = array(o, "rows"))
                for (guint i = 0; i < json_array_get_length(rows); i++) {
                    JsonNode* row = json_array_get_element(rows, i);
                    b.rows.push_back(strings(JSON_NODE_HOLDS_ARRAY(row) ? json_node_get_array(row) : nullptr));
                }
            if (JsonArray* items = array(o, "items"))
                for (guint i = 0; i < json_array_get_length(items); i++) {
                    JsonNode* item = json_array_get_element(items, i);
                    if (JSON_NODE_HOLDS_OBJECT(item)) {
                        b.items.push_back(parse_block(json_node_get_object(item)));
                        b.items.back().type = "row";
                    }
                }
            return b;
        }

        PluginPill parse_pill(JsonObject* o) { return {str(o, "text"), str(o, "icon"), str(o, "image")}; }
        PluginChip parse_chip(JsonObject* o) { return {str(o, "text"), str(o, "icon")}; }
        PluginTile parse_tile(JsonObject* o) { return {str(o, "title"), str(o, "icon"), str(o, "image")}; }

        PluginPage parse_page(JsonObject* o) {
            PluginPage page{str(o, "title"), {}};
            if (JsonArray* blocks = array(o, "blocks"))
                for (guint i = 0; i < json_array_get_length(blocks); i++) {
                    JsonNode* block = json_array_get_element(blocks, i);
                    if (JSON_NODE_HOLDS_OBJECT(block))
                        page.blocks.push_back(parse_block(json_node_get_object(block)));
                }
            return page;
        }

        // True when `key` is present and changed the slot: an object replaces it, null clears it.
        template <typename T, typename Parse>
        bool update(std::optional<T>& slot, JsonObject* o, const char* key, Parse parse) {
            JsonNode* n = json_object_get_member(o, key);
            if (!n)
                return false;
            std::optional<T> next;
            if (JSON_NODE_HOLDS_OBJECT(n))
                next = parse(json_node_get_object(n));
            else if (!JSON_NODE_HOLDS_NULL(n))
                return false;
            if (next == slot)
                return false;
            slot = std::move(next);
            return true;
        }

    } // namespace

    unsigned apply_plugin_line(PluginState& state, const std::string& line, bool* valid) {
        JsonParser* parser = json_parser_new();
        JsonNode* root = json_parser_load_from_data(parser, line.data(), static_cast<gssize>(line.size()), nullptr)
                             ? json_parser_get_root(parser)
                             : nullptr;
        const bool ok = root && JSON_NODE_HOLDS_OBJECT(root);
        if (valid)
            *valid = ok;
        unsigned changed = 0;
        if (ok) {
            JsonObject* o = json_node_get_object(root);
            changed |= update(state.pill, o, "pill", parse_pill) ? SLOT_PILL : 0;
            changed |= update(state.chip, o, "chip", parse_chip) ? SLOT_CHIP : 0;
            changed |= update(state.tile, o, "tile", parse_tile) ? SLOT_TILE : 0;
            changed |= update(state.page, o, "page", parse_page) ? SLOT_PAGE : 0;
        }
        g_object_unref(parser);
        return changed;
    }

    std::string plugin_event(const char* event, const std::string& id) {
        JsonBuilder* builder = json_builder_new();
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "event");
        json_builder_add_string_value(builder, event);
        if (!id.empty()) {
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, id.c_str());
        }
        json_builder_end_object(builder);
        JsonNode* root = json_builder_get_root(builder);
        JsonGenerator* generator = json_generator_new();
        json_generator_set_root(generator, root);
        gchar* data = json_generator_to_data(generator, nullptr);
        std::string out = data;
        g_free(data);
        g_object_unref(generator);
        json_node_unref(root);
        g_object_unref(builder);
        return out;
    }

    Plugin::Plugin(std::string name, std::string command)
        : name_(std::move(name)), command_(std::move(command)), cancellable_(g_cancellable_new()) {}

    Plugin::~Plugin() {
        if (restart_id_)
            g_source_remove(restart_id_);
        g_cancellable_cancel(cancellable_);
        if (process_)
            g_subprocess_send_signal(process_, SIGTERM);
        g_clear_object(&stdout_);
        g_clear_object(&stderr_);
        g_clear_object(&process_);
        g_object_unref(cancellable_);
    }

    void Plugin::start() { spawn(); }

    void Plugin::spawn() {
        GSubprocessLauncher* launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
        // GIO ignores SIGPIPE process-wide; a plugin's own pipelines (curl | jq) expect the default.
        g_subprocess_launcher_set_child_setup(launcher, [](gpointer) { signal(SIGPIPE, SIG_DFL); }, nullptr, nullptr);
        GError* err = nullptr;
        process_ = g_subprocess_launcher_spawn(launcher, &err, "/bin/sh", "-c", command_.c_str(), nullptr);
        g_object_unref(launcher);
        if (!process_) {
            g_warning("plugin %s: %s", name_.c_str(), err->message);
            g_error_free(err);
            return;
        }
        started_us_ = g_get_monotonic_time();

        stdin_fd_ = g_unix_output_stream_get_fd(G_UNIX_OUTPUT_STREAM(g_subprocess_get_stdin_pipe(process_)));
        // A plugin that never reads its stdin must not block the bar once the pipe fills.
        fcntl(stdin_fd_, F_SETFL, fcntl(stdin_fd_, F_GETFL) | O_NONBLOCK);

        stdout_ = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process_));
        stderr_ = g_data_input_stream_new(g_subprocess_get_stderr_pipe(process_));
        read_next(stdout_);
        read_next(stderr_);
        g_subprocess_wait_async(process_, cancellable_, on_wait, this);
    }

    void Plugin::send(const std::string& line) {
        if (stdin_fd_ < 0)
            return;
        const std::string buf = line + '\n';
        // ponytail: one write, dropped on EAGAIN; a partial write into a nearly full pipe garbles one line.
        if (write(stdin_fd_, buf.data(), buf.size()) < 0)
            g_debug("plugin %s: event dropped", name_.c_str());
    }

    void Plugin::read_next(GDataInputStream* stream) {
        g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT, cancellable_, on_line, this);
    }

    void Plugin::on_line(GObject* source, GAsyncResult* result, gpointer data) {
        GError* err = nullptr;
        gsize length = 0;
        char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &err);
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { // the plugin is being destroyed
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Plugin*>(data);
        const bool is_stdout = source == G_OBJECT(self->stdout_);
        const bool is_stderr = source == G_OBJECT(self->stderr_);
        if (err)
            g_error_free(err);
        if (!line || (!is_stdout && !is_stderr)) { // EOF, or a stream from a run that already exited
            g_free(line);
            return;
        }

        if (is_stderr) {
            g_message("plugin %s: %s", self->name_.c_str(), line);
        } else if (length > 0) {
            bool valid = true;
            const unsigned changed = apply_plugin_line(self->state_, std::string(line, length), &valid);
            if (!valid)
                g_warning("plugin %s: not a JSON object: %.200s", self->name_.c_str(), line);
            if (changed && self->listener_)
                self->listener_(changed);
        }
        g_free(line);
        self->read_next(G_DATA_INPUT_STREAM(source));
    }

    void Plugin::on_wait(GObject* source, GAsyncResult* result, gpointer data) {
        GError* err = nullptr;
        const bool ok = g_subprocess_wait_finish(G_SUBPROCESS(source), result, &err);
        if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(err);
            return;
        }
        if (err)
            g_error_free(err);
        static_cast<Plugin*>(data)->exited();
    }

    void Plugin::exited() {
        const int status = g_subprocess_get_if_exited(process_) ? g_subprocess_get_exit_status(process_) : -1;
        if (g_get_monotonic_time() - started_us_ >= HEALTHY_US)
            backoff_s_ = 1;
        g_warning("plugin %s: exited (%d), restarting in %us", name_.c_str(), status, backoff_s_);

        stdin_fd_ = -1;
        g_clear_object(&stdout_);
        g_clear_object(&stderr_);
        g_clear_object(&process_);

        unsigned cleared = (state_.pill ? SLOT_PILL : 0) | (state_.chip ? SLOT_CHIP : 0) |
                           (state_.tile ? SLOT_TILE : 0) | (state_.page ? SLOT_PAGE : 0);
        state_ = {};
        if (cleared && listener_)
            listener_(cleared);

        restart_id_ = g_timeout_add_seconds(backoff_s_, on_restart, this);
        backoff_s_ = std::min(backoff_s_ * 2, MAX_BACKOFF_S);
    }

    gboolean Plugin::on_restart(gpointer data) {
        auto* self = static_cast<Plugin*>(data);
        self->restart_id_ = 0;
        self->spawn();
        return G_SOURCE_REMOVE;
    }

} // namespace fenriz::bar
