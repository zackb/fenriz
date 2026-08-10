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
        void focus_first();
        bool matches(GtkFlowBoxChild* child) const;
        GtkFlowBoxChild* first_match() const;
        GtkFlowBoxChild* current() const;
        void activate(GtkFlowBoxChild* child);

        static gboolean on_thumbnail(gpointer data);
        static gboolean on_filter(GtkFlowBoxChild* child, gpointer data);
        static void on_search_changed(GtkEditable* editable, gpointer data);
        static void on_child_activated(GtkFlowBox* box, GtkFlowBoxChild* child, gpointer data);
        static gboolean on_key(GtkEventControllerKey* c, guint keyval, guint code, GdkModifierType state, gpointer d);

        Config& cfg_;
        Background& background_;
        std::vector<std::string> paths_;
        std::vector<GtkWidget*> pictures_;
        size_t next_ = 0; // next path_ needing a thumbnail
        guint thumbnail_source_ = 0;
        GtkWindow* window_ = nullptr;
        GtkWidget* search_ = nullptr;
        GtkWidget* scroll_ = nullptr;
        GtkWidget* grid_ = nullptr;
        GtkFlowBoxChild* current_child_ = nullptr;
    };

} // namespace fenriz::desktop
