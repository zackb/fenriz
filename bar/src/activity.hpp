#pragma once

#include <optional>
#include <string>

namespace fenriz::bar {

    // What the collapsed pill shows besides the clock. A timed activity holds the pill until it expires or something of
    // equal or higher priority replaces it. A sticky one (low battery) waits underneath and returns whenever nothing
    // timed is showing, until it is dismissed.
    struct Activity {
        std::string child; // the pill stack page to show
        int priority = 0;
        bool sticky = false;
    };

    class ActivityQueue {
    public:
        // Priorities, highest first.
        static constexpr int LEVEL = 3; // a volume or brightness key
        static constexpr int EVENT = 2; // a charger, a device
        static constexpr int MEDIA = 1; // a track change

        // True when `a` takes the pill now; a timed activity that loses is dropped.
        bool push(const Activity& a) {
            if (a.sticky) {
                sticky_ = a;
                return !current_;
            }
            if (current_ && current_->priority > a.priority)
                return false;
            current_ = a;
            return true;
        }

        // The timed activity ran out. Returns what the pill shows next: the sticky one, or "clock".
        std::string expire() {
            current_.reset();
            return showing();
        }

        void dismiss_sticky() { sticky_.reset(); }

        std::string showing() const {
            if (current_)
                return current_->child;
            return sticky_ ? sticky_->child : "clock";
        }

        int current_priority() const { return current_ ? current_->priority : 0; }

    private:
        std::optional<Activity> current_;
        std::optional<Activity> sticky_;
    };

} // namespace fenriz::bar
