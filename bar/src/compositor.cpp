#include "compositor.hpp"

#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

namespace fenriz::bar {

    namespace {

        constexpr guint RETRY_SECONDS = 2; // ponytail: fixed interval, backoff if a missing compositor ever matters

        std::string string_member(JsonObject* obj, const char* name) {
            if (!json_object_has_member(obj, name))
                return "";
            const char* s = json_object_get_string_member_with_default(obj, name, "");
            return s ? s : "";
        }

        std::vector<int> int_array(JsonObject* obj, const char* name) {
            std::vector<int> out;
            if (!json_object_has_member(obj, name))
                return out;
            JsonArray* arr = json_object_get_array_member(obj, name);
            if (!arr)
                return out;
            for (guint i = 0; i < json_array_get_length(arr); i++)
                out.push_back(static_cast<int>(json_array_get_int_element(arr, i)));
            return out;
        }

    } // namespace

    int CompositorState::active_on(const std::string& output) const {
        for (const Output& o : outputs)
            if (o.name == output)
                return o.active;
        return 0;
    }

    std::string CompositorState::focused_output() const {
        for (const Output& o : outputs)
            if (o.focused)
                return o.name;
        return "";
    }

    std::optional<CompositorState> parse_state(const std::string& line) {
        JsonParser* parser = json_parser_new();
        std::optional<CompositorState> result;
        JsonNode* root = nullptr;
        if (json_parser_load_from_data(parser, line.data(), static_cast<gssize>(line.size()), nullptr))
            root = json_parser_get_root(parser);

        JsonObject* obj = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
        if (obj && json_object_has_member(obj, "workspaces")) {
            CompositorState s;
            if (JsonArray* outputs = json_object_get_array_member(obj, "outputs"))
                for (guint i = 0; i < json_array_get_length(outputs); i++) {
                    JsonObject* o = json_array_get_object_element(outputs, i);
                    if (!o)
                        continue;
                    s.outputs.push_back({string_member(o, "name"),
                                         static_cast<int>(json_object_get_int_member_with_default(o, "active", 0)),
                                         json_object_get_boolean_member_with_default(o, "focused", false) != FALSE});
                }
            if (JsonObject* ws = json_object_get_object_member(obj, "workspaces")) {
                s.occupied = int_array(ws, "occupied");
                s.urgent = int_array(ws, "urgent");
            }
            JsonNode* active = json_object_get_member(obj, "activeWindow");
            if (active && JSON_NODE_HOLDS_OBJECT(active)) {
                JsonObject* w = json_node_get_object(active);
                s.has_window = true;
                s.title = string_member(w, "title");
                s.app_id = string_member(w, "appId");
                s.icon = string_member(w, "icon");
            }
            result = std::move(s);
        }
        g_object_unref(parser);
        return result;
    }

    Compositor::~Compositor() {
        if (retry_id_)
            g_source_remove(retry_id_);
        disconnect();
    }

    void Compositor::start(Listener listener) {
        listener_ = std::move(listener);
        const char* path = g_getenv("FENRIZ_SOCKET");
        if (!path || !*path) {
            g_message("compositor: FENRIZ_SOCKET is not set, so no workspaces (is this fenriz?)");
            return;
        }
        path_ = path;
        connect();
    }

    void Compositor::connect() {
        cancel_ = g_cancellable_new();
        GSocketClient* client = g_socket_client_new();
        GSocketAddress* addr = g_unix_socket_address_new(path_.c_str());
        g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(addr), cancel_, on_connected, this);
        g_object_unref(addr);
        g_object_unref(client);
    }

    void Compositor::disconnect() {
        if (cancel_) {
            g_cancellable_cancel(cancel_);
            g_clear_object(&cancel_);
        }
        g_clear_object(&in_);
        if (conn_) {
            g_io_stream_close(G_IO_STREAM(conn_), nullptr, nullptr);
            g_clear_object(&conn_);
        }
    }

    void Compositor::on_connected(GObject* source, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GSocketConnection* conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), res, &err);
        if (!conn) {
            // cancelled means `data` may already be gone
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                auto* self = static_cast<Compositor*>(data);
                g_message("compositor: %s: %s", self->path_.c_str(), err->message);
                self->disconnect();
                self->retry_id_ = g_timeout_add_seconds(RETRY_SECONDS, on_retry, self);
            }
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Compositor*>(data);
        self->conn_ = conn;
        self->in_ = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
        g_message("compositor: connected to %s", self->path_.c_str());
        self->read_next();
    }

    void Compositor::read_next() {
        g_data_input_stream_read_line_async(in_, G_PRIORITY_DEFAULT, cancel_, on_line, this);
    }

    void Compositor::on_line(GObject* source, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        gsize len = 0;
        char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), res, &len, &err);
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(err);
            return;
        }
        auto* self = static_cast<Compositor*>(data);
        if (!line) {
            g_message("compositor: feed closed%s%s", err ? ": " : "", err ? err->message : "");
            g_clear_error(&err);
            self->disconnect();
            self->publish({});
            self->retry_id_ = g_timeout_add_seconds(RETRY_SECONDS, on_retry, self);
            return;
        }
        if (auto state = parse_state(std::string(line, len)))
            self->publish(std::move(*state));
        g_free(line);
        self->read_next();
    }

    gboolean Compositor::on_retry(gpointer data) {
        auto* self = static_cast<Compositor*>(data);
        self->retry_id_ = 0;
        self->connect();
        return G_SOURCE_REMOVE;
    }

    void Compositor::publish(CompositorState state) {
        if (state == state_)
            return;
        state_ = std::move(state);
        if (listener_)
            listener_(state_);
    }

    void Compositor::workspace(int n) { send("{\"cmd\":\"workspace\",\"n\":" + std::to_string(n) + "}\n"); }

    void Compositor::exit() { send("{\"cmd\":\"exit\"}\n"); }

    void Compositor::send(const std::string& line) {
        if (!conn_)
            return;
        GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn_));
        GError* err = nullptr;
        if (!g_output_stream_write_all(out, line.data(), line.size(), nullptr, nullptr, &err)) {
            g_warning("compositor: %s", err->message);
            g_error_free(err);
        }
    }

} // namespace fenriz::bar
