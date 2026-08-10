#pragma once

#include <functional>
#include <memory>
#include <vector>

struct ext_idle_notification_v1;
struct ext_idle_notifier_v1;
struct wl_seat;

namespace fenriz::desktop {

    struct IdleWatch;

    // ext-idle-notify-v1 watcher.
    // Several timeouts can run at once: dim, then lock, then blank.
    class Idle {
    public:
        using Handler = std::function<void()>;

        Idle();
        ~Idle();

        Idle(const Idle&) = delete;
        Idle& operator=(const Idle&) = delete;

        // False when the compositor has no ext-idle-notify-v1
        bool start();

        // Ignored for seconds <= 0
        void watch(int seconds, Handler idled, Handler resumed);

    private:
        ext_idle_notifier_v1* notifier_ = nullptr;
        wl_seat* seat_ = nullptr;
        std::vector<std::unique_ptr<IdleWatch>> watches_;
    };

} // namespace fenriz::desktop
