#pragma once

struct wlr_surface;
struct wlr_box;
struct wlr_scene_tree;
struct wlr_scene_node;
struct wlr_scene_blur;

namespace fenriz {

    class Server;

    // Blur behind a surface, driven by the client
    // ext-background-effect-v1 and org_kde_kwin_blur
    namespace background_blur {

        constexpr int RECTS_MAX = 16;

        void init(Server& server);

        // Re-send the capability set (config reload) and re-apply the blur parameters.
        void reload(Server& server);

        // (Re)build `nodes` so there is one blur node per rectangle of `surface`'s committed blur
        // region, each placed in `parent` directly below `below`, offset by (ox,oy).
        void place(wlr_surface* surface,
                   wlr_scene_tree* parent,
                   wlr_scene_node* below,
                   int ox,
                   int oy,
                   const wlr_box& content,
                   int radius,
                   wlr_scene_blur* nodes[RECTS_MAX]);

        // Forget the nodes without destroying them.
        void forget(wlr_scene_blur* nodes[RECTS_MAX]);

        // Destroy the nodes, for a caller whose blur nodes are siblings of its surface's subtree rather than children
        // of it.
        void clear(wlr_scene_blur* nodes[RECTS_MAX]);

    } // namespace background_blur

} // namespace fenriz
