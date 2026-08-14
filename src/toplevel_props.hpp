#pragma once

struct wlr_xdg_toplevel;

namespace fenriz {

    class Server;

    // Client-declared properties of a window that are neither its title nor its app_id:
    // xdg-toplevel-tag-v1's tag and xdg-toplevel-icon-v1's icon name.
    namespace toplevel_props {

        void init(Server& server);

        // What the client declared for this toplevel, or "" if it declared nothing.
        const char* tag_of(const wlr_xdg_toplevel* toplevel);
        const char* icon_of(const wlr_xdg_toplevel* toplevel);

    } // namespace toplevel_props

} // namespace fenriz
