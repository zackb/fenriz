#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

#include "config.hpp"
#include "frecency.hpp"

namespace fenriz::desktop {

    // Application launcher: desktop entries, matched by name and ranked by frecency.
    class Launcher {
    public:
        explicit Launcher(const Config& cfg);
        ~Launcher();

        Launcher(const Launcher&) = delete;
        Launcher& operator=(const Launcher&) = delete;

        void toggle(GtkApplication* app);
        void close();

    private:
        struct Entry {
            GAppInfo* info = nullptr; // owned
            std::string id;
            std::string name;
        };

        void build(GtkApplication* app);
        void load_entries();
        void refilter();
        void activate_row(GtkListBoxRow* row);
        void move_selection(int delta);

        static void on_search_changed(GtkEditable* editable, gpointer data);
        static void on_row_activated(GtkListBox* box, GtkListBoxRow* row, gpointer data);
        static gboolean on_key(GtkEventControllerKey* c, guint keyval, guint code, GdkModifierType state, gpointer d);

        const Config& cfg_;
        UsageStore usage_;
        std::vector<Entry> entries_;
        std::vector<int> shown_; // indices into entries_, in display order
        GtkWindow* window_ = nullptr;
        GtkWidget* search_ = nullptr;
        GtkWidget* list_ = nullptr;
    };

} // namespace fenriz::desktop
