#pragma once

#include <gtk/gtk.h>

struct zwp_idle_inhibitor_v1;

namespace fenriz::bar {

    // Keeps the session awake through idle-inhibit-unstable-v1, so it works with any idle daemon the compositor
    // informs (fenriz-desktop, swayidle, hypridle).
    //
    // An inhibitor belongs to a wl_surface, and a layer surface moved to another screen gets a new one, so the
    // inhibitor is re-made each time `window` maps.
    class IdleInhibitor {
    public:
        explicit IdleInhibitor(GtkWindow* window);
        ~IdleInhibitor();

        IdleInhibitor(const IdleInhibitor&) = delete;
        IdleInhibitor& operator=(const IdleInhibitor&) = delete;

        // False when the compositor does not offer the protocol.
        bool available() const;
        bool active() const { return wanted_; }
        void set(bool on);

    private:
        void apply();

        GtkWindow* window_;
        zwp_idle_inhibitor_v1* inhibitor_ = nullptr;
        bool wanted_ = false;
        gulong map_handler_ = 0;
    };

} // namespace fenriz::bar
