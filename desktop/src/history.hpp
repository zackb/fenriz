#pragma once

#include <gtk/gtk.h>

#include "config.hpp"
#include "notify.hpp"

namespace fenriz::desktop {

    // Panel listing the notifications that have already come and gone.
    class History {
    public:
        History(const Config& cfg, Notifications& notifications);
        ~History();

        History(const History&) = delete;
        History& operator=(const History&) = delete;

        void toggle(GtkApplication* app);
        void close();

    private:
        void build(GtkApplication* app);
        void refill();

        static gboolean dismiss_idle(gpointer data);
        static void on_dismiss(GtkButton* button, gpointer data);
        static void on_clear(GtkButton* button, gpointer data);
        static gboolean on_key(GtkEventControllerKey* c, guint keyval, guint code, GdkModifierType state, gpointer d);

        const Config& cfg_;
        Notifications& notifications_;
        GtkWindow* window_ = nullptr;
        GtkWidget* list_ = nullptr;
        GtkWidget* scroll_ = nullptr;
        GtkWidget* empty_ = nullptr;
    };

} // namespace fenriz::desktop
