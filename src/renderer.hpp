#pragma once

struct wlr_buffer;

namespace fenriz {

    class Server;

    namespace renderer {
        // Round the corners of every mapped window by overdrawing the corner regions of
        // its box with the background color `bg` (RGBA, 0..1). This is a GLES2-only pass
        // that draws directly into the just-rendered output `buffer`'s FBO; it is a no-op
        // when the renderer isn't GLES2, when `buffer` is null, or when rounding is 0.
        // Must run after wlr_render_pass_submit and before wlr_output_commit_state.
        void round_corners(Server& server, wlr_buffer* buffer, int output_w, int output_h, const float bg[4]);
    } // namespace renderer

} // namespace fenriz
