#include "workspace_protocol.hpp"

#include <string>
#include <vector>

#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::workspace_protocol {

    namespace {

        struct State {
            Server* server = nullptr;
            wlr_ext_workspace_manager_v1* manager = nullptr;
            wlr_ext_workspace_group_handle_v1* group = nullptr;
            std::vector<wlr_ext_workspace_handle_v1*> handles; // 0-based workspace
            wl_listener commit;
        };

        State state;

        void on_commit(wl_listener* listener, void* data) {
            (void)listener;
            auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);

            wlr_ext_workspace_v1_request* req;
            wl_list_for_each(req, event->requests, link) {
                if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !req->activate.workspace)
                    continue;
                for (size_t i = 0; i < state.handles.size(); i++) {
                    if (state.handles[i] != req->activate.workspace)
                        continue;
                    set_workspace(*state.server, (int)i);
                    break;
                }
            }
        }

    } // namespace

    void init(Server& server) {
        state.server = &server;
        state.manager = wlr_ext_workspace_manager_v1_create(server.display, 1);
        state.group = wlr_ext_workspace_group_handle_v1_create(state.manager, 0);
        add_listener(state.commit, state.manager->events.commit, on_commit);
    }

    void sync(Server& server) {
        if (!state.manager)
            return;

        const size_t want = (size_t)server.config.workspaces;
        while (state.handles.size() > want) {
            wlr_ext_workspace_handle_v1_destroy(state.handles.back());
            state.handles.pop_back();
        }
        while (state.handles.size() < want) {
            const std::string name = std::to_string(state.handles.size() + 1);
            wlr_ext_workspace_handle_v1* h = wlr_ext_workspace_handle_v1_create(
                state.manager, name.c_str(), EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
            wlr_ext_workspace_handle_v1_set_name(h, name.c_str());
            const uint32_t coord = (uint32_t)state.handles.size();
            wlr_ext_workspace_handle_v1_set_coordinates(h, &coord, 1);
            wlr_ext_workspace_handle_v1_set_group(h, state.group);
            state.handles.push_back(h);
        }

        for (size_t i = 0; i < state.handles.size(); i++) {
            const int ws = (int)i;

            // every screen shows one workspace, so several are active at once on multi-monitor.
            bool active = false;
            for (output::Output* o : server.outputs)
                if (o->enabled && o->active_ws == ws)
                    active = true;

            bool urgent = false;
            for (View* v : server.views)
                if (v->mapped && v->urgent && v->workspace == ws)
                    urgent = true;

            wlr_ext_workspace_handle_v1_set_active(state.handles[i], active);
            wlr_ext_workspace_handle_v1_set_urgent(state.handles[i], urgent);
        }
    }

    void output_enter(wlr_output* out) {
        if (state.group)
            wlr_ext_workspace_group_handle_v1_output_enter(state.group, out);
    }

    void output_leave(wlr_output* out) {
        if (state.group)
            wlr_ext_workspace_group_handle_v1_output_leave(state.group, out);
    }

} // namespace fenriz::workspace_protocol
