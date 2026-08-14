#include "toplevel_props.hpp"

#include <string>
#include <vector>

#include "ipc.hpp"
#include "server.hpp"
#include "wlr.hpp"

namespace fenriz::toplevel_props {

    namespace {

        // One per toplevel that declared something.
        struct Entry {
            wlr_xdg_toplevel* toplevel;
            std::string tag;
            std::string icon;
            wl_listener destroy;
        };

        struct State {
            Server* server = nullptr;
            std::vector<Entry*> entries;
            wl_listener set_tag;
            wl_listener set_icon;
        };

        State state;

        Entry* find(const wlr_xdg_toplevel* toplevel) {
            for (Entry* e : state.entries)
                if (e->toplevel == toplevel)
                    return e;
            return nullptr;
        }

        void on_toplevel_destroy(wl_listener* listener, void*) {
            Entry* e = wl_container_of(listener, e, destroy);
            wl_list_remove(&e->destroy.link);
            std::erase(state.entries, e);
            delete e;
        }

        Entry* entry_for(wlr_xdg_toplevel* toplevel) {
            if (Entry* e = find(toplevel))
                return e;
            Entry* e = new Entry{};
            e->toplevel = toplevel;
            add_listener(e->destroy, toplevel->events.destroy, on_toplevel_destroy);
            state.entries.push_back(e);
            return e;
        }

        void on_set_tag(wl_listener*, void* data) {
            auto* event = static_cast<wlr_xdg_toplevel_tag_manager_v1_set_tag_event*>(data);
            entry_for(event->toplevel)->tag = event->tag ? event->tag : "";
            ipc::publish(*state.server);
        }

        void on_set_icon(wl_listener*, void* data) {
            auto* event = static_cast<wlr_xdg_toplevel_icon_manager_v1_set_icon_event*>(data);
            entry_for(event->toplevel)->icon = event->icon && event->icon->name ? event->icon->name : "";
            ipc::publish(*state.server);
        }

    } // namespace

    void init(Server& server) {
        state.server = &server;

        // xdg-toplevel-tag-v1
        wlr_xdg_toplevel_tag_manager_v1* tags = wlr_xdg_toplevel_tag_manager_v1_create(server.display, 1);
        add_listener(state.set_tag, tags->events.set_tag, on_set_tag);
        wlr_xdg_toplevel_icon_manager_v1* icons = wlr_xdg_toplevel_icon_manager_v1_create(server.display, 1);
        add_listener(state.set_icon, icons->events.set_icon, on_set_icon);
    }

    const char* tag_of(const wlr_xdg_toplevel* toplevel) {
        Entry* e = toplevel ? find(toplevel) : nullptr;
        return e ? e->tag.c_str() : "";
    }

    const char* icon_of(const wlr_xdg_toplevel* toplevel) {
        Entry* e = toplevel ? find(toplevel) : nullptr;
        return e ? e->icon.c_str() : "";
    }

} // namespace fenriz::toplevel_props
