#pragma once

struct wlr_output;

namespace fenriz {

    class Server;

    // ext-workspace-v1: the standard protocol bars (waybar's `ext/workspaces`, and friends) use to list workspaces and
    // click one to switch.
    namespace workspace_protocol {

        // Create the ext_workspace_manager_v1 global and the workspace handles.
        void init(Server& server);

        // Push current workspace state (active, urgent, count) to the protocol.
        void sync(Server& server);

        // Attach/detach an output from the workspace group.
        void output_enter(wlr_output* out);
        void output_leave(wlr_output* out);

    } // namespace workspace_protocol

} // namespace fenriz
