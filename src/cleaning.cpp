#include "cleaning.hpp"

#include <cctype>

#include "config.hpp"
#include "ipc.hpp"
#include "server.hpp"
#include "wlr.hpp"

namespace fenriz::cleaning {

    namespace {

        int expire(void* data) {
            stop(*static_cast<Server*>(data));
            return 0;
        }

    } // namespace

    void start(Server& server, int seconds) {
        if (server.locked || seconds <= 0)
            return;

        server.cleaning = true;
        server.cleaning_seconds = seconds;

        if (server.repeat_timer)
            wl_event_source_timer_update(server.repeat_timer, 0);
        server.repeat_keycode = 0;

        if (!server.cleaning_timer) {
            wl_event_loop* loop = wl_display_get_event_loop(server.display);
            server.cleaning_timer = wl_event_loop_add_timer(loop, expire, &server);
        }
        wl_event_source_timer_update(server.cleaning_timer, seconds * 1000);

        wlr_log(WLR_INFO, "fenriz: cleaning mode, input off for %ds", seconds);
        ipc::cleaning(server, seconds, cancel_bind(server));
    }

    void stop(Server& server) {
        if (!server.cleaning)
            return;
        server.cleaning = false;
        server.cleaning_seconds = 0;
        if (server.cleaning_timer)
            wl_event_source_timer_update(server.cleaning_timer, 0);

        if (wlr_keyboard* kb = wlr_seat_get_keyboard(server.seat))
            wlr_seat_keyboard_notify_modifiers(server.seat, &kb->modifiers);

        wlr_log(WLR_INFO, "fenriz: cleaning mode over");
        ipc::cleaning(server, 0, "");
    }

    std::string cancel_bind(const Server& server) {
        for (const Bind& b : server.config.binds) {
            if (b.action != Action::Cleaning)
                continue;
            std::string s;
            if (b.mods & WLR_MODIFIER_LOGO)
                s += "SUPER+";
            if (b.mods & WLR_MODIFIER_CTRL)
                s += "CTRL+";
            if (b.mods & WLR_MODIFIER_ALT)
                s += "ALT+";
            if (b.mods & WLR_MODIFIER_SHIFT)
                s += "SHIFT+";
            char name[64] = {0};
            xkb_keysym_get_name(b.sym, name, sizeof(name));
            if (name[0] && !name[1])
                name[0] = (char)std::toupper((unsigned char)name[0]);
            return s + name;
        }
        return "";
    }

} // namespace fenriz::cleaning
