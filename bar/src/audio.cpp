#include "audio.hpp"

#include <algorithm>

namespace fenriz::bar {

    namespace {

        // mixer-api reads back the old level for ~40 ms after a set, so our own write wins for this long; otherwise a
        // dragged slider snaps back under the pointer.
        constexpr gint64 STALE_AFTER_US = 500000;

        constexpr int SCALE_CUBIC = 1; // same curve as wpctl and pavucontrol

    } // namespace

    Audio::Audio() = default;

    Audio::~Audio() {
        g_clear_object(&nodes_);
        g_clear_object(&mixer_);
        g_clear_object(&defaults_);
        if (core_) {
            wp_core_disconnect(core_);
            g_clear_object(&core_);
        }
    }

    bool Audio::start() {
        // not WP_INIT_ALL: that replaces the GLib log writer, and a second writer aborts the process
        wp_init(static_cast<WpInitFlags>(WP_INIT_PIPEWIRE | WP_INIT_SPA_TYPES));
        core_ = wp_core_new(g_main_context_get_thread_default(), nullptr, nullptr);
        if (!wp_core_connect(core_)) {
            g_clear_object(&core_);
            g_message("audio: no PipeWire");
            return false;
        }

        nodes_ = wp_object_manager_new();
        wp_object_manager_add_interest(nodes_,
                                       WP_TYPE_NODE,
                                       WP_CONSTRAINT_TYPE_PW_PROPERTY,
                                       "media.class",
                                       "c(ss)",
                                       "Audio/Sink",
                                       "Audio/Source",
                                       nullptr);
        wp_object_manager_request_object_features(nodes_, WP_TYPE_NODE, WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
        g_signal_connect_swapped(nodes_, "objects-changed", G_CALLBACK(on_changed), this);
        wp_core_install_object_manager(core_, nodes_);

        pending_ = 2;
        for (const char* module : {"libwireplumber-module-default-nodes-api", "libwireplumber-module-mixer-api"})
            wp_core_load_component(
                core_, module, "module", nullptr, nullptr, nullptr, (GAsyncReadyCallback)on_loaded, this);
        return true;
    }

    void Audio::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void Audio::on_loaded(WpCore* core, GAsyncResult* res, gpointer data) {
        auto* self = static_cast<Audio*>(data);
        GError* err = nullptr;
        if (!wp_core_load_component_finish(core, res, &err)) {
            g_warning("audio: %s", err ? err->message : "component failed to load");
            g_clear_error(&err);
            return;
        }
        if (--self->pending_ != 0)
            return;

        self->mixer_ = wp_plugin_find(core, "mixer-api");
        self->defaults_ = wp_plugin_find(core, "default-nodes-api");
        if (!self->mixer_ || !self->defaults_) {
            g_warning("audio: wireplumber is missing the mixer plugins");
            return;
        }
        g_object_set(self->mixer_, "scale", SCALE_CUBIC, nullptr);

        self->pending_ = 2;
        for (WpPlugin* plugin : {self->mixer_, self->defaults_})
            wp_object_activate(
                WP_OBJECT(plugin), WP_PLUGIN_FEATURE_ENABLED, nullptr, (GAsyncReadyCallback)on_activated, self);
    }

    void Audio::on_activated(WpObject* obj, GAsyncResult* res, gpointer data) {
        auto* self = static_cast<Audio*>(data);
        GError* err = nullptr;
        if (!wp_object_activate_finish(obj, res, &err)) {
            g_warning("audio: plugin activation failed: %s", err ? err->message : "unknown");
            g_clear_error(&err);
            return;
        }
        if (--self->pending_ != 0)
            return;
        self->ready_ = true;
        g_signal_connect(self->mixer_, "changed", G_CALLBACK(on_mixer_changed), self);
        g_signal_connect_swapped(self->defaults_, "changed", G_CALLBACK(on_changed), self);
        self->refresh();
    }

    void Audio::on_mixer_changed(WpPlugin*, guint id, gpointer data) {
        auto* self = static_cast<Audio*>(data);
        if (id == self->sink_.id || id == self->source_.id)
            self->refresh();
    }

    void Audio::on_changed(gpointer data) { static_cast<Audio*>(data)->refresh(); }

    void Audio::read(Endpoint& ep) {
        ep.devices.clear();
        WpIterator* it = wp_object_manager_new_filtered_iterator(
            nodes_, WP_TYPE_NODE, WP_CONSTRAINT_TYPE_PW_PROPERTY, "media.class", "=s", ep.media_class, nullptr);
        GValue item = G_VALUE_INIT;
        while (wp_iterator_next(it, &item)) {
            auto* obj = WP_PIPEWIRE_OBJECT(g_value_get_object(&item));
            const char* name = wp_pipewire_object_get_property(obj, "node.name");
            const char* desc = wp_pipewire_object_get_property(obj, "node.description");
            if (!desc)
                desc = wp_pipewire_object_get_property(obj, "node.nick");
            if (name)
                ep.devices.push_back({wp_proxy_get_bound_id(WP_PROXY(obj)), name, desc ? desc : name});
            g_value_unset(&item);
        }
        wp_iterator_unref(it);
        std::sort(ep.devices.begin(), ep.devices.end(), [](const Device& a, const Device& b) {
            return a.description < b.description;
        });

        if (!ready_)
            return;
        guint32 id = 0;
        g_signal_emit_by_name(defaults_, "get-default-node", ep.media_class, &id);
        ep.id = id == G_MAXUINT32 ? 0 : id;
        if (ep.id == 0) {
            ep.percent = -1;
            return;
        }
        if (g_get_monotonic_time() - ep.set_at < STALE_AFTER_US)
            return;

        GVariant* current = nullptr;
        g_signal_emit_by_name(mixer_, "get-volume", ep.id, &current);
        if (!current)
            return;
        double level = 0;
        gboolean muted = FALSE;
        g_variant_lookup(current, "volume", "d", &level);
        g_variant_lookup(current, "mute", "b", &muted);
        g_variant_unref(current);
        ep.percent = static_cast<int>(std::clamp(level, 0.0, 1.0) * 100 + 0.5);
        ep.muted = muted;
    }

    void Audio::refresh() {
        read(sink_);
        read(source_);
        for (auto& listener : listeners_)
            listener();
    }

    void Audio::write(Endpoint& ep, GVariant* dict) {
        if (!ready_ || ep.id == 0) {
            g_variant_unref(g_variant_ref_sink(dict));
            return;
        }
        ep.set_at = g_get_monotonic_time();
        gboolean ok = FALSE;
        g_signal_emit_by_name(mixer_, "set-volume", ep.id, dict, &ok);
        if (!ok)
            g_warning("audio: set-volume on node %u failed", ep.id);
        for (auto& listener : listeners_)
            listener();
    }

    void Audio::set_percent(bool source, int percent) {
        Endpoint& ep = source ? source_ : sink_;
        ep.percent = std::clamp(percent, 0, 100);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "volume", g_variant_new_double(ep.percent / 100.0));
        write(ep, g_variant_builder_end(&b));
    }

    void Audio::set_muted(bool source, bool muted) {
        Endpoint& ep = source ? source_ : sink_;
        ep.muted = muted;
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "mute", g_variant_new_boolean(muted));
        write(ep, g_variant_builder_end(&b));
    }

    void Audio::set_default(bool source, const std::string& node_name) {
        if (!ready_)
            return;
        gboolean ok = FALSE;
        g_signal_emit_by_name(defaults_,
                              "set-default-configured-node-name",
                              source ? "Audio/Source" : "Audio/Sink",
                              node_name.c_str(),
                              &ok);
        if (!ok)
            g_warning("audio: could not make %s the default", node_name.c_str());
    }

} // namespace fenriz::bar
