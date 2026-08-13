#include "notify.hpp"

#include <gtk/gtk.h>

namespace fenriz::desktop {

    namespace {

        constexpr const char* BUS_NAME = "org.freedesktop.Notifications";
        constexpr const char* OBJECT_PATH = "/org/freedesktop/Notifications";

        constexpr guint8 URGENCY_CRITICAL = 2;

        constexpr guint32 REASON_CLOSED_BY_CALL = 3;

        constexpr const char* INTROSPECTION_XML = R"(<node>
  <interface name='org.freedesktop.Notifications'>
    <method name='Notify'>
      <arg type='s'    name='app_name'       direction='in'/>
      <arg type='u'    name='replaces_id'    direction='in'/>
      <arg type='s'    name='app_icon'       direction='in'/>
      <arg type='s'    name='summary'        direction='in'/>
      <arg type='s'    name='body'           direction='in'/>
      <arg type='as'   name='actions'        direction='in'/>
      <arg type='a{sv}' name='hints'         direction='in'/>
      <arg type='i'    name='expire_timeout' direction='in'/>
      <arg type='u'    name='id'             direction='out'/>
    </method>
    <method name='CloseNotification'>
      <arg type='u' name='id' direction='in'/>
    </method>
    <method name='GetCapabilities'>
      <arg type='as' name='capabilities' direction='out'/>
    </method>
    <method name='GetServerInformation'>
      <arg type='s' name='name'         direction='out'/>
      <arg type='s' name='vendor'       direction='out'/>
      <arg type='s' name='version'      direction='out'/>
      <arg type='s' name='spec_version' direction='out'/>
    </method>
    <signal name='NotificationClosed'>
      <arg type='u' name='id'/>
      <arg type='u' name='reason'/>
    </signal>
    <signal name='ActionInvoked'>
      <arg type='u' name='id'/>
      <arg type='s' name='action_key'/>
    </signal>
  </interface>
</node>)";

        constexpr const char* CAPABILITIES[] = {"body", "body-markup", "actions", "icon-static", nullptr};

        void on_method_call(GDBusConnection*,
                            const gchar*,
                            const gchar*,
                            const gchar*,
                            const gchar* method,
                            GVariant* params,
                            GDBusMethodInvocation* invocation,
                            gpointer data) {
            static_cast<Notifications*>(data)->handle_call(method, params, invocation);
        }

        const GDBusInterfaceVTable VTABLE = {on_method_call, nullptr, nullptr, {nullptr}};

        void bus_acquired_cb(GDBusConnection* bus, const gchar*, gpointer data) {
            static_cast<Notifications*>(data)->on_bus_acquired(bus);
        }

        void name_acquired_cb(GDBusConnection*, const gchar* name, gpointer) { g_message("notify: owning %s", name); }

        void name_lost_cb(GDBusConnection*, const gchar* name, gpointer) {
            g_warning("notify: could not own %s, another notification daemon holds it "
                      "(mako, dunst, swaync?); set notifications = off to stop trying",
                      name);
        }

        std::string hint_string(GVariant* hints, const char* key) {
            GVariant* value = g_variant_lookup_value(hints, key, G_VARIANT_TYPE_STRING);
            if (!value)
                return "";
            std::string out = g_variant_get_string(value, nullptr);
            g_variant_unref(value);
            return out;
        }

        guint8 hint_urgency(GVariant* hints) {
            GVariant* value = g_variant_lookup_value(hints, "urgency", G_VARIANT_TYPE_BYTE);
            if (!value)
                return 1;
            const guint8 urgency = g_variant_get_byte(value);
            g_variant_unref(value);
            return urgency;
        }

        GdkTexture* hint_texture(GVariant* hints) {
            GVariant* value = nullptr;
            for (const char* key : {"image-data", "image_data", "icon_data"})
                if ((value = g_variant_lookup_value(hints, key, G_VARIANT_TYPE("(iiibiiay)"))))
                    break;
            if (!value)
                return nullptr;

            gint width = 0, height = 0, rowstride = 0, bits = 0, channels = 0;
            gboolean has_alpha = FALSE;
            GVariant* pixels = nullptr;
            g_variant_get(value, "(iiibii@ay)", &width, &height, &rowstride, &has_alpha, &bits, &channels, &pixels);

            GdkTexture* texture = nullptr;
            gsize length = 0;
            const guchar* data = static_cast<const guchar*>(g_variant_get_fixed_array(pixels, &length, 1));
            const gint want_channels = has_alpha ? 4 : 3;
            // 8 bits per sample only
            const bool sane = data && bits == 8 && channels == want_channels && width > 0 && height > 0 &&
                              rowstride >= width * channels &&
                              length >= static_cast<gsize>(rowstride) * (height - 1) + width * channels;
            if (sane) {
                GBytes* bytes = g_bytes_new(data, length);
                texture = gdk_memory_texture_new(width,
                                                 height,
                                                 has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                 bytes,
                                                 static_cast<gsize>(rowstride));
                g_bytes_unref(bytes);
            } else {
                g_warning("notify: ignoring an unusable image hint (%dx%d, %d bits, %d channels)",
                          width,
                          height,
                          bits,
                          channels);
            }

            g_variant_unref(pixels);
            g_variant_unref(value);
            return texture;
        }

    } // namespace

    int notify_expiry_ms(int client_timeout, guint8 urgency, int default_ms) {
        if (client_timeout > 0)
            return client_timeout;
        if (client_timeout == 0)
            return 0;
        return urgency >= URGENCY_CRITICAL ? 0 : default_ms;
    }

    std::string notify_body_markup(const std::string& body) {
        if (body.empty())
            return "";
        if (pango_parse_markup(body.c_str(), -1, 0, nullptr, nullptr, nullptr, nullptr))
            return body;
        char* escaped = g_markup_escape_text(body.c_str(), -1);
        std::string out = escaped;
        g_free(escaped);
        return out;
    }

    Actions notify_split_actions(const std::vector<std::string>& flat) {
        Actions actions;
        for (size_t i = 0; i + 1 < flat.size(); i += 2) {
            if (flat[i] == "default") {
                actions.has_default = true;
                continue;
            }
            actions.buttons.emplace_back(flat[i], flat[i + 1]);
        }
        return actions;
    }

    Notifications::Notifications(GtkApplication* app, const Config& cfg)
        : app_(app)
        , default_timeout_(cfg.notify_timeout)
        , toasts_(
              cfg.notify_position,
              [this](guint32 id, const std::string& key) {
                  emit("ActionInvoked", g_variant_new("(us)", id, key.c_str()));
              },
              [this](guint32 id, guint32 reason) { emit("NotificationClosed", g_variant_new("(uu)", id, reason)); }) {}

    Notifications::~Notifications() {
        if (owner_id_)
            g_bus_unown_name(owner_id_);
        if (introspection_)
            g_dbus_node_info_unref(introspection_);
    }

    void Notifications::start() {
        GError* err = nullptr;
        introspection_ = g_dbus_node_info_new_for_xml(INTROSPECTION_XML, &err);
        if (!introspection_) {
            g_warning("notify: bad introspection XML: %s", err ? err->message : "unknown");
            g_clear_error(&err);
            return;
        }
        owner_id_ = g_bus_own_name(G_BUS_TYPE_SESSION,
                                   BUS_NAME,
                                   G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                   bus_acquired_cb,
                                   name_acquired_cb,
                                   name_lost_cb,
                                   this,
                                   nullptr);
    }

    void Notifications::on_bus_acquired(GDBusConnection* bus) {
        bus_ = bus;
        GDBusInterfaceInfo* iface = g_dbus_node_info_lookup_interface(introspection_, BUS_NAME);
        GError* err = nullptr;
        if (!g_dbus_connection_register_object(bus, OBJECT_PATH, iface, &VTABLE, this, nullptr, &err)) {
            g_warning("notify: register %s: %s", OBJECT_PATH, err ? err->message : "unknown");
            g_clear_error(&err);
        }
    }

    void Notifications::emit(const char* signal, GVariant* params) {
        if (!bus_) {
            g_variant_unref(g_variant_ref_sink(params));
            return;
        }
        GError* err = nullptr;
        if (!g_dbus_connection_emit_signal(bus_, nullptr, OBJECT_PATH, BUS_NAME, signal, params, &err)) {
            g_warning("notify: emit %s: %s", signal, err ? err->message : "unknown");
            g_clear_error(&err);
        }
    }

    guint32 Notifications::notify(GVariant* params) {
        const char* app_name = nullptr;
        const char* app_icon = nullptr;
        const char* summary = nullptr;
        const char* body = nullptr;
        guint32 replaces_id = 0;
        gint expire_timeout = -1;
        GVariant* raw_actions = nullptr;
        GVariant* hints = nullptr;
        g_variant_get(params,
                      "(&su&s&s&s@as@a{sv}i)",
                      &app_name,
                      &replaces_id,
                      &app_icon,
                      &summary,
                      &body,
                      &raw_actions,
                      &hints,
                      &expire_timeout);

        std::vector<std::string> flat;
        GVariantIter iter;
        g_variant_iter_init(&iter, raw_actions);
        for (const char* entry = nullptr; g_variant_iter_next(&iter, "&s", &entry);)
            flat.emplace_back(entry ? entry : "");
        const Actions actions = notify_split_actions(flat);

        const guint8 urgency = hint_urgency(hints);

        Toast toast;
        toast.id = replaces_id ? replaces_id : ++counter_;
        toast.summary = summary ? summary : "";
        toast.body = notify_body_markup(body ? body : "");
        toast.actions = actions.buttons;
        toast.has_default = actions.has_default;
        toast.critical = urgency >= URGENCY_CRITICAL;
        toast.expiry_ms = notify_expiry_ms(expire_timeout, urgency, default_timeout_);

        toast.texture = hint_texture(hints);
        if (!toast.texture) {
            toast.icon = hint_string(hints, "image-path");
            if (toast.icon.empty())
                toast.icon = hint_string(hints, "image_path");
            if (toast.icon.empty() && app_icon)
                toast.icon = app_icon;
        }

        if (replaces_id > counter_)
            counter_ = replaces_id;

        toasts_.show(app_, toast);

        if (toast.texture)
            g_object_unref(toast.texture);
        g_variant_unref(raw_actions);
        g_variant_unref(hints);
        return toast.id;
    }

    void Notifications::handle_call(const char* method, GVariant* params, GDBusMethodInvocation* invocation) {
        if (g_strcmp0(method, "Notify") == 0) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", notify(params)));
        } else if (g_strcmp0(method, "CloseNotification") == 0) {
            guint32 id = 0;
            g_variant_get(params, "(u)", &id);
            g_dbus_method_invocation_return_value(invocation, nullptr);
            toasts_.close(id, REASON_CLOSED_BY_CALL);
        } else if (g_strcmp0(method, "GetCapabilities") == 0) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", CAPABILITIES));
        } else if (g_strcmp0(method, "GetServerInformation") == 0) {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(ssss)", "fenriz-desktop", "fenriz", FENRIZ_DESKTOP_VERSION, "1.2"));
        } else {
            g_dbus_method_invocation_return_error_literal(
                invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no such method");
        }
    }

} // namespace fenriz::desktop
