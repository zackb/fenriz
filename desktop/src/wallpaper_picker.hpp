#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

#include "background.hpp"
#include "config.hpp"

namespace fenriz::desktop {

    // A grid of thumbnails from cfg.wallpaper_dir. Picking one applies it to every monitor and records it in XDG state.
    class WallpaperPicker {
    public:
        WallpaperPicker(Config& cfg, Background& background);
        ~WallpaperPicker();

        WallpaperPicker(const WallpaperPicker&) = delete;
        WallpaperPicker& operator=(const WallpaperPicker&) = delete;

        void toggle(GtkApplication* app);
        void close();

    private:
        void build(GtkApplication* app);
        void load();
        void move(GtkMovementStep step, int count);
        void focus_first();
        bool matches(GtkFlowBoxChild* child) const;
        GtkFlowBoxChild* first_match() const;
        GtkFlowBoxChild* current() const;
        void activate(GtkFlowBoxChild* child);

        static gboolean on_thumbnail(gpointer data);
        static gboolean on_filter(GtkFlowBoxChild* child, gpointer data);
        static void on_search_changed(GtkEditable* editable, gpointer data);
        static void on_child_activated(GtkFlowBox* box, GtkFlowBoxChild* child, gpointer data);
        static void on_grid_map(GtkWidget* grid, gpointer data);
        static gboolean on_place_cursor(gpointer data);
        static gboolean on_key(GtkEventControllerKey* c, guint keyval, guint code, GdkModifierType state, gpointer d);

        Config& cfg_;
        Background& background_;
        std::vector<std::string> paths_;
        std::vector<GtkWidget*> pictures_;
        size_t next_ = 0; // next path_ needing a thumbnail
        guint thumbnail_source_ = 0;
        guint place_source_ = 0;
        bool pending_g_ = false;
        GtkWindow* window_ = nullptr;
        GtkWidget* search_ = nullptr; // null in vim mode, which is how the two modes are told apart
        GtkWidget* scroll_ = nullptr;
        GtkWidget* grid_ = nullptr;
        int current_index_ = -1; // grid position of the wallpaper in use, -1 if it is not in the directory
    };

} // namespace fenriz::desktop
