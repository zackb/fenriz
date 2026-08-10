#pragma once

#include <functional>

#include "config.hpp"

struct ext_idle_notification_v1;
struct ext_idle_notifier_v1;

namespace fenriz::desktop {

    // ext-idle-notify-v1 watcher.
    class Idle {
    public:
        using Handler = std::function<void()>;

        explicit Idle(const Config& cfg);
        ~Idle();

        Idle(const Idle&) = delete;
        Idle& operator=(const Idle&) = delete;

        void start(Handler on_idle);

    private:
        const Config& cfg_;
        Handler on_idle_;
        ext_idle_notifier_v1* notifier_ = nullptr;
        ext_idle_notification_v1* notification_ = nullptr;

        friend struct IdleListener;
    };

} // namespace fenriz::desktop
