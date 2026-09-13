#include "tray.hpp"

#include <unistd.h>

#include <algorithm>

namespace fenriz::bar {

    namespace {

        constexpr const char* WATCHER_NAME = "org.kde.StatusNotifierWatcher";
        constexpr const char* WATCHER_PATH = "/StatusNotifierWatcher";
        constexpr const char* ITEM_IFACE = "org.kde.StatusNotifierItem";
        constexpr const char* MENU_IFACE = "com.canonical.dbusmenu";
        constexpr int PIXMAP_SIZE = 32; // enough for a 16px icon at 2x

        constexpr const char* WATCHER_XML = R"xml(
<node>
  <interface name="org.kde.StatusNotifierWatcher">
    <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
    <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
    <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
    <property name="ProtocolVersion" type="i" access="read"/>
    <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
    <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
    <signal name="StatusNotifierHostRegistered"/>
  </interface>
</node>)xml";

        void call(GDBusConnection* bus,
                  const std::string& dest,
                  const std::string& path,
                  const char* iface,
                  const char* method,
                  GVariant* args) {
            g_dbus_connection_call(bus,
                                   dest.c_str(),
                                   path.c_str(),
                                   iface,
                                   method,
                                   args,
                                   nullptr,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   2000,
                                   nullptr,
                                   nullptr,
                                   nullptr);
        }

        // IconPixmap: a(iiay) of ARGB32 in network byte order, which is GDK's A8R8G8B8 as it stands.
        GdkTexture* pixmap_texture(GVariant* pixmaps) {
            std::vector<int> sizes;
            GVariantIter it;
            g_variant_iter_init(&it, pixmaps);
            gint32 w = 0, h = 0;
            GVariant* data = nullptr;
            while (g_variant_iter_next(&it, "(ii@ay)", &w, &h, &data)) {
                sizes.push_back(std::min(w, h));
                g_variant_unref(data);
            }
            const int pick = pick_pixmap(sizes, PIXMAP_SIZE);
            if (pick < 0)
                return nullptr;
            g_variant_get_child(pixmaps, pick, "(ii@ay)", &w, &h, &data);
            gsize len = 0;
            const auto* bytes = static_cast<const guint8*>(g_variant_get_fixed_array(data, &len, 1));
            GdkTexture* texture = nullptr;
            if (w > 0 && h > 0 && len == static_cast<gsize>(w) * h * 4) {
                GBytes* copy = g_bytes_new(bytes, len);
                texture = gdk_memory_texture_new(w, h, GDK_MEMORY_A8R8G8B8, copy, w * 4);
                g_bytes_unref(copy);
            }
            g_variant_unref(data);
            return texture;
        }

    } // namespace

    ItemAddress parse_item_address(const std::string& service, const std::string& sender) {
        if (!service.empty() && service[0] == '/')
            return {sender, service};
        const size_t slash = service.find('/');
        if (slash == std::string::npos)
            return {service, "/StatusNotifierItem"};
        return {service.substr(0, slash), service.substr(slash)};
    }

    int pick_pixmap(const std::vector<int>& sizes, int wanted) {
        int best = -1;
        for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
            if (best < 0) {
                best = i;
                continue;
            }
            const bool fits = sizes[i] >= wanted, best_fits = sizes[best] >= wanted;
            if ((fits && (!best_fits || sizes[i] < sizes[best])) || (!fits && !best_fits && sizes[i] > sizes[best]))
                best = i;
        }
        return best;
    }

    bool has_menu(const std::string& path) { return !path.empty() && path != "/" && path != "/NO_DBUSMENU"; }

    Tray::~Tray() {
        if (cancel_) {
            g_cancellable_cancel(cancel_);
            g_object_unref(cancel_);
        }
        for (auto& item : items_)
            g_dbus_connection_signal_unsubscribe(bus_, item->subscription);
        for (guint id : subscriptions_)
            g_dbus_connection_signal_unsubscribe(bus_, id);
        if (owner_changed_)
            g_dbus_connection_signal_unsubscribe(bus_, owner_changed_);
        if (watch_id_)
            g_bus_unwatch_name(watch_id_);
        if (watcher_name_)
            g_bus_unown_name(watcher_name_);
        if (host_name_)
            g_bus_unown_name(host_name_);
        if (watcher_object_)
            g_dbus_connection_unregister_object(bus_, watcher_object_);
    }

    // Whoever holds the watcher name decides the mode: nobody means we take it, someone else means we are a host of
    // theirs. A watcher that goes away (quickshell restarting) hands the name back to us, and apps re-register with
    // whichever watcher appears next.
    void Tray::start(GDBusConnection* bus) {
        if (!bus)
            return;
        bus_ = bus;
        cancel_ = g_cancellable_new();

        GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(WATCHER_XML, nullptr);
        static const GDBusInterfaceVTable vtable = {on_watcher_method, on_watcher_property, nullptr, {}};
        watcher_object_ =
            g_dbus_connection_register_object(bus_, WATCHER_PATH, info->interfaces[0], &vtable, this, nullptr, nullptr);
        g_dbus_node_info_unref(info);

        owner_changed_ = g_dbus_connection_signal_subscribe(
            bus_,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "NameOwnerChanged",
            "/org/freedesktop/DBus",
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* params, gpointer data) {
                const char* name = nullptr;
                const char* old_owner = nullptr;
                const char* new_owner = nullptr;
                g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
                if (!*new_owner)
                    static_cast<Tray*>(data)->remove(name);
            },
            this,
            nullptr);

        for (const char* signal : {"StatusNotifierItemRegistered", "StatusNotifierItemUnregistered"})
            subscriptions_.push_back(g_dbus_connection_signal_subscribe(
                bus_,
                WATCHER_NAME,
                WATCHER_NAME,
                signal,
                WATCHER_PATH,
                nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                +[](GDBusConnection*,
                    const char*,
                    const char*,
                    const char*,
                    const char* sig,
                    GVariant* params,
                    gpointer data) {
                    auto* self = static_cast<Tray*>(data);
                    if (self->is_watcher_)
                        return; // our own signals, already handled
                    const char* service = nullptr;
                    g_variant_get(params, "(&s)", &service);
                    const ItemAddress a = parse_item_address(service, "");
                    if (g_str_equal(sig, "StatusNotifierItemRegistered"))
                        self->add(service, "");
                    else
                        self->remove(a.bus);
                },
                this,
                nullptr));

        watch_id_ = g_bus_watch_name_on_connection(
            bus_,
            WATCHER_NAME,
            G_BUS_NAME_WATCHER_FLAGS_NONE,
            +[](GDBusConnection* conn, const char*, const char* owner, gpointer data) {
                auto* self = static_cast<Tray*>(data);
                if (self->owner_ == owner)
                    return; // GLib can report the same owner twice
                self->owner_ = owner;
                self->is_watcher_ = g_strcmp0(owner, g_dbus_connection_get_unique_name(conn)) == 0;
                if (!self->is_watcher_)
                    self->become_host();
                else
                    g_message("tray: acting as the StatusNotifierWatcher");
            },
            +[](GDBusConnection*, const char*, gpointer data) {
                auto* self = static_cast<Tray*>(data);
                self->owner_.clear();
                for (auto& item : self->items_)
                    g_dbus_connection_signal_unsubscribe(self->bus_, item->subscription);
                self->items_.clear();
                self->notify();
                if (self->watcher_name_)
                    g_bus_unown_name(self->watcher_name_);
                // not queued: if someone else gets there first, the appeared handler makes us their host
                self->watcher_name_ = g_bus_own_name_on_connection(
                    self->bus_, WATCHER_NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, nullptr, nullptr, nullptr, nullptr);
            },
            this,
            nullptr);
    }

    void Tray::become_host() {
        if (!host_name_) {
            const std::string name = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
            host_name_ = g_bus_own_name_on_connection(
                bus_, name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
        }
        const std::string name = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
        call(bus_,
             WATCHER_NAME,
             WATCHER_PATH,
             WATCHER_NAME,
             "RegisterStatusNotifierHost",
             g_variant_new("(s)", name.c_str()));
        g_message("tray: hosting for the existing StatusNotifierWatcher");

        g_dbus_connection_call(
            bus_,
            WATCHER_NAME,
            WATCHER_PATH,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", WATCHER_NAME, "RegisteredStatusNotifierItems"),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            2000,
            cancel_,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr);
                if (!reply)
                    return;
                auto* self = static_cast<Tray*>(data);
                GVariant* v = nullptr;
                g_variant_get(reply, "(v)", &v);
                if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
                    gsize n = 0;
                    const char** list = g_variant_get_strv(v, &n);
                    for (gsize i = 0; i < n; i++)
                        self->add(list[i], "");
                    g_free(list);
                }
                g_variant_unref(v);
                g_variant_unref(reply);
            },
            this);
    }

    void Tray::subscribe(std::function<void()> listener) { listeners_.push_back(std::move(listener)); }

    void Tray::notify() {
        for (auto& l : listeners_)
            l();
    }

    TrayItem* Tray::find(const std::string& key) {
        for (auto& item : items_)
            if (item->key == key)
                return item.get();
        return nullptr;
    }

    void Tray::emit(const char* signal, const std::string& service) {
        g_dbus_connection_emit_signal(
            bus_, nullptr, WATCHER_PATH, WATCHER_NAME, signal, g_variant_new("(s)", service.c_str()), nullptr);
    }

    void Tray::add(const std::string& service, const std::string& sender) {
        const ItemAddress a = parse_item_address(service, sender);
        if (a.bus.empty() || find(a.bus + a.path))
            return;
        auto item = std::make_unique<TrayItem>();
        item->bus = a.bus;
        item->path = a.path;
        item->key = a.bus + a.path;
        const std::string key = item->key;
        item->subscription = g_dbus_connection_signal_subscribe(
            bus_,
            a.bus.c_str(),
            ITEM_IFACE,
            nullptr, // NewIcon, NewStatus, NewTitle, ...: none carry the new value, so re-read them all
            a.path.c_str(),
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
                const auto* target = static_cast<std::pair<Tray*, std::string>*>(data);
                target->first->refresh(target->second);
            },
            new std::pair<Tray*, std::string>(this, key),
            +[](gpointer data) { delete static_cast<std::pair<Tray*, std::string>*>(data); });
        items_.push_back(std::move(item));
        if (is_watcher_)
            emit("StatusNotifierItemRegistered", key);
        refresh(key);
    }

    void Tray::remove(const std::string& bus) {
        const auto gone = std::remove_if(items_.begin(), items_.end(), [&](const std::unique_ptr<TrayItem>& item) {
            if (item->bus != bus)
                return false;
            g_dbus_connection_signal_unsubscribe(bus_, item->subscription);
            if (is_watcher_)
                emit("StatusNotifierItemUnregistered", item->key);
            return true;
        });
        if (gone == items_.end())
            return;
        items_.erase(gone, items_.end());
        notify();
    }

    void Tray::refresh(const std::string& key) {
        TrayItem* item = find(key);
        if (!item)
            return;
        auto* request = new std::pair<Tray*, std::string>(this, key);
        g_dbus_connection_call(
            bus_,
            item->bus.c_str(),
            item->path.c_str(),
            "org.freedesktop.DBus.Properties",
            "GetAll",
            g_variant_new("(s)", ITEM_IFACE),
            G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE,
            2000,
            cancel_,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                std::unique_ptr<std::pair<Tray*, std::string>> req(static_cast<std::pair<Tray*, std::string>*>(data));
                GError* err = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
                if (!reply) {
                    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_message("tray: %s: %s", req->second.c_str(), err->message);
                    g_error_free(err);
                    return;
                }
                TrayItem* item = req->first->find(req->second);
                GVariant* props = g_variant_get_child_value(reply, 0);
                if (item) {
                    const char* s = nullptr;
                    gboolean b = FALSE;
                    if (g_variant_lookup(props, "Title", "&s", &s))
                        item->title = s;
                    if (g_variant_lookup(props, "Status", "&s", &s))
                        item->status = s;
                    if (g_variant_lookup(props, "IconName", "&s", &s))
                        item->icon_name = s;
                    if (g_variant_lookup(props, "AttentionIconName", "&s", &s))
                        item->attention_icon_name = s;
                    if (g_variant_lookup(props, "IconThemePath", "&s", &s))
                        item->theme_path = s;
                    if (g_variant_lookup(props, "Menu", "&o", &s))
                        item->menu_path = s;
                    if (g_variant_lookup(props, "ItemIsMenu", "b", &b))
                        item->item_is_menu = b;
                    g_clear_object(&item->pixmap);
                    if (GVariant* pixmaps = g_variant_lookup_value(props, "IconPixmap", G_VARIANT_TYPE("a(iiay)"))) {
                        item->pixmap = pixmap_texture(pixmaps);
                        g_variant_unref(pixmaps);
                    }
                    req->first->notify();
                }
                g_variant_unref(props);
                g_variant_unref(reply);
            },
            request);
    }

    void Tray::activate(const TrayItem& item, int x, int y) {
        call(bus_, item.bus, item.path, ITEM_IFACE, "Activate", g_variant_new("(ii)", x, y));
    }

    void Tray::secondary_activate(const TrayItem& item, int x, int y) {
        call(bus_, item.bus, item.path, ITEM_IFACE, "SecondaryActivate", g_variant_new("(ii)", x, y));
    }

    void Tray::context_menu(const TrayItem& item, int x, int y) {
        call(bus_, item.bus, item.path, ITEM_IFACE, "ContextMenu", g_variant_new("(ii)", x, y));
    }

    void Tray::scroll(const TrayItem& item, int delta, bool horizontal) {
        call(bus_,
             item.bus,
             item.path,
             ITEM_IFACE,
             "Scroll",
             g_variant_new("(is)", delta, horizontal ? "horizontal" : "vertical"));
    }

    void Tray::fetch_menu(const TrayItem& item, std::function<void(GVariant*)> done) {
        if (!has_menu(item.menu_path))
            return;
        // lets apps that build their menu lazily fill it in; the reply does not matter
        call(bus_, item.bus, item.menu_path, MENU_IFACE, "AboutToShow", g_variant_new("(i)", 0));
        g_dbus_connection_call(
            bus_,
            item.bus.c_str(),
            item.menu_path.c_str(),
            MENU_IFACE,
            "GetLayout",
            g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0)),
            G_VARIANT_TYPE("(u(ia{sv}av))"),
            G_DBUS_CALL_FLAGS_NONE,
            2000,
            cancel_,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                std::unique_ptr<std::function<void(GVariant*)>> done(
                    static_cast<std::function<void(GVariant*)>*>(data));
                GError* err = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
                if (!reply) {
                    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_message("tray: menu: %s", err->message);
                    g_error_free(err);
                    return;
                }
                GVariant* layout = g_variant_get_child_value(reply, 1);
                (*done)(layout);
                g_variant_unref(layout);
                g_variant_unref(reply);
            },
            new std::function<void(GVariant*)>(std::move(done)));
    }

    void Tray::menu_clicked(const TrayItem& item, int id) {
        call(bus_,
             item.bus,
             item.menu_path,
             MENU_IFACE,
             "Event",
             g_variant_new(
                 "(isvu)", id, "clicked", g_variant_new_int32(0), static_cast<guint32>(g_get_real_time() / 1000)));
    }

    void Tray::on_watcher_method(GDBusConnection*,
                                 const char* sender,
                                 const char*,
                                 const char*,
                                 const char* method,
                                 GVariant* params,
                                 GDBusMethodInvocation* invocation,
                                 gpointer data) {
        auto* self = static_cast<Tray*>(data);
        const char* arg = nullptr;
        g_variant_get(params, "(&s)", &arg);
        if (g_str_equal(method, "RegisterStatusNotifierItem")) {
            self->add(arg, sender);
        } else {
            g_dbus_connection_emit_signal(
                self->bus_, nullptr, WATCHER_PATH, WATCHER_NAME, "StatusNotifierHostRegistered", nullptr, nullptr);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    GVariant* Tray::on_watcher_property(
        GDBusConnection*, const char*, const char*, const char*, const char* property, GError**, gpointer data) {
        auto* self = static_cast<Tray*>(data);
        if (g_str_equal(property, "RegisteredStatusNotifierItems")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE_STRING_ARRAY);
            for (const auto& item : self->items_)
                g_variant_builder_add(&b, "s", item->key.c_str());
            return g_variant_builder_end(&b);
        }
        if (g_str_equal(property, "IsStatusNotifierHostRegistered"))
            return g_variant_new_boolean(TRUE);
        return g_variant_new_int32(0);
    }

} // namespace fenriz::bar
