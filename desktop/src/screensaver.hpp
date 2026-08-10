#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fenriz::desktop {

    // Music alone should not keep the screen awake. Video players say "video" in the reason.
    bool screensaver_should_inhibit(std::string_view reason);

    // org.freedesktop.ScreenSaver (and org.gnome.ScreenSaver) idle-inhibit broker.
    //
    // Apps have two ways to keep the screen awake: the Wayland idle-inhibit protocol, which
    // the compositor handles, and this DBus interface, which browsers and VLC prefer. Firefox
    // only falls back to Wayland when this name is unowned, so without it video never inhibits.
    class Screensaver {
    public:
        // Called on every transition between "nothing inhibiting" and "something inhibiting".
        using Handler = std::function<void(bool inhibited)>;

        explicit Screensaver(Handler on_change);
        ~Screensaver();

        Screensaver(const Screensaver&) = delete;
        Screensaver& operator=(const Screensaver&) = delete;

        void start();

        void handle_call(GDBusConnection* bus,
                         const char* sender,
                         const char* method,
                         GVariant* params,
                         GDBusMethodInvocation* invocation);
        void on_bus_acquired(GDBusConnection* bus, const char* name);
        void drop_peer(const char* peer);

    private:
        void notify();

        Handler on_change_;
        // cookie -> the caller's unique bus name
        std::unordered_map<guint32, std::string> inhibitors_;
        std::vector<guint> owner_ids_;
        GDBusNodeInfo* introspection_ = nullptr;
        GDBusConnection* bus_ = nullptr;
        guint name_watch_ = 0;
        guint32 counter_ = 0;
        bool inhibited_ = false;
    };

} // namespace fenriz::desktop
