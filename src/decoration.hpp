#pragma once

namespace fenriz {

    class Server;

    namespace decoration {

        // Create the xdg-decoration manager and force every toplevel to server-side mode,
        // so clients (kitty, etc.) drop their own titlebars — fenriz draws the border.
        void init(Server& server);

    } // namespace decoration

} // namespace fenriz
