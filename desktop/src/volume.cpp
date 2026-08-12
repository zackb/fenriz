#include "volume.hpp"

#include <algorithm>

namespace fenriz::desktop {

    namespace {

        // mixer-api's cache is stale by about one key-repeat
        // resync once the mixer has certainly caught up.
        constexpr gint64 STALE_AFTER_US = 1000000;

        constexpr int SCALE_CUBIC = 1;

        constexpr double MAX_LEVEL = 1.0;

    } // namespace

    double volume_step(double current, int delta) { return std::clamp(current + delta / 100.0, 0.0, MAX_LEVEL); }

    int volume_percent(double level) { return static_cast<int>(std::clamp(level, 0.0, MAX_LEVEL) * 100 + 0.5); }

    Volume::Volume() = default;

    Volume::~Volume() {
        g_clear_object(&mixer_);
        g_clear_object(&defaults_);
        if (core_) {
            wp_core_disconnect(core_);
            g_clear_object(&core_);
        }
    }

    void Volume::on_activated(WpObject* obj, GAsyncResult* res, gpointer data) {
        auto* self = static_cast<Volume*>(data);
        GError* err = nullptr;
        if (!wp_object_activate_finish(obj, res, &err)) {
            g_warning("volume: plugin activation failed: %s", err ? err->message : "unknown");
            g_clear_error(&err);
            return;
        }
        if (--self->pending_ == 0)
            self->ready_ = true;
    }

    void Volume::on_loaded(WpCore* core, GAsyncResult* res, gpointer data) {
        auto* self = static_cast<Volume*>(data);
        GError* err = nullptr;
        if (!wp_core_load_component_finish(core, res, &err)) {
            g_warning("volume: %s", err ? err->message : "component failed to load");
            g_clear_error(&err);
            return;
        }
        if (--self->pending_ != 0)
            return;

        self->mixer_ = wp_plugin_find(core, "mixer-api");
        self->defaults_ = wp_plugin_find(core, "default-nodes-api");
        if (!self->mixer_ || !self->defaults_) {
            g_warning("volume: wireplumber is missing the mixer plugins");
            return;
        }
        g_object_set(self->mixer_, "scale", SCALE_CUBIC, nullptr);

        self->pending_ = 2;
        wp_object_activate(
            WP_OBJECT(self->mixer_), WP_PLUGIN_FEATURE_ENABLED, nullptr, (GAsyncReadyCallback)on_activated, self);
        wp_object_activate(
            WP_OBJECT(self->defaults_), WP_PLUGIN_FEATURE_ENABLED, nullptr, (GAsyncReadyCallback)on_activated, self);
    }

    bool Volume::start() {
        wp_init(static_cast<WpInitFlags>(WP_INIT_PIPEWIRE | WP_INIT_SPA_TYPES));
        // shares main loop, every callback below is on the gtk thread
        core_ = wp_core_new(g_main_context_get_thread_default(), nullptr, nullptr);
        if (!wp_core_connect(core_)) {
            g_clear_object(&core_);
            return false;
        }
        pending_ = 2;
        wp_core_load_component(core_,
                               "libwireplumber-module-default-nodes-api",
                               "module",
                               nullptr,
                               nullptr,
                               nullptr,
                               (GAsyncReadyCallback)on_loaded,
                               this);
        wp_core_load_component(core_,
                               "libwireplumber-module-mixer-api",
                               "module",
                               nullptr,
                               nullptr,
                               nullptr,
                               (GAsyncReadyCallback)on_loaded,
                               this);
        return true;
    }

    int Volume::apply(Node& node, int delta, bool toggle) {
        if (!ready_)
            return -1;

        guint32 id = 0;
        g_signal_emit_by_name(defaults_, "get-default-node", node.media_class, &id);
        if (id == 0 || id == G_MAXUINT32)
            return -1;

        const gint64 now = g_get_monotonic_time();
        if (now - node.at > STALE_AFTER_US || node.level < 0) {
            GVariant* current = nullptr;
            g_signal_emit_by_name(mixer_, "get-volume", id, &current);
            if (!current)
                return -1;
            gboolean muted = FALSE;
            g_variant_lookup(current, "volume", "d", &node.level);
            g_variant_lookup(current, "mute", "b", &muted);
            node.muted = muted;
            g_variant_unref(current);
        }
        node.at = now;

        if (toggle)
            node.muted = !node.muted;
        else
            node.level = volume_step(node.level, delta);

        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "mute", g_variant_new_boolean(node.muted));
        if (!toggle)
            g_variant_builder_add(&b, "{sv}", "volume", g_variant_new_double(node.level));

        gboolean ok = FALSE;
        g_signal_emit_by_name(mixer_, "set-volume", id, g_variant_builder_end(&b), &ok);
        if (!ok)
            return -1;

        return volume_percent(node.level);
    }

    int Volume::adjust(int delta) { return apply(sink_, delta, false); }

    int Volume::toggle_mute() { return apply(sink_, 0, true); }

    int Volume::toggle_mic_mute() { return apply(source_, 0, true); }

} // namespace fenriz::desktop
