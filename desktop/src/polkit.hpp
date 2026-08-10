#pragma once

#include <gtk/gtk.h>

#include "config.hpp"

namespace fenriz::desktop {

    // PolicyKit authentication agent.
    //
    // Unlike the lock screen this does NOT run its own PAM stack: polkitd only trusts an
    // answer that came through PolkitAgentSession, which drives polkit-agent-helper-1 itself.
    // Our job is to show the prompt and relay what the user types.
    class Polkit {
    public:
        explicit Polkit(const Config& cfg);
        ~Polkit();

        Polkit(const Polkit&) = delete;
        Polkit& operator=(const Polkit&) = delete;

        // Warns and stays inactive if another agent already owns this session.
        void start();

    private:
        const Config& cfg_;
        void* listener_ = nullptr;     // PolkitAgentListener*
        void* registration_ = nullptr; // opaque handle from polkit_agent_listener_register
    };

} // namespace fenriz::desktop
