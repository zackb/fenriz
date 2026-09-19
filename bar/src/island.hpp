#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "activity.hpp"
#include "spring.hpp"

namespace fenriz::bar {

    // The pill at the top-center of one screen that morphs open into pages.
    //
    // Its layer surface is resized only when a morph needs more room than it has, and shrunk once the morph has
    // settled; every frame in between only redraws and moves the input region. Resizing a layer surface per frame
    // would configure-storm the compositor.
    class Island {
    public:
        Island() = default;
        ~Island();

        Island(const Island&) = delete;
        Island& operator=(const Island&) = delete;

        void start(GtkApplication* app);

        // Opens `page` on `monitor` (null keeps the current one). The page already open closes instead.
        void toggle(const std::string& page, GdkMonitor* monitor);
        void open(const std::string& page, GdkMonitor* monitor);
        void close();

        // Re-creates the surface on `monitor`, which also stacks it above surfaces made since (a new screen's bar).
        void remap(GdkMonitor* monitor);
        GdkMonitor* monitor() const { return monitor_; }

        // Pages contributed by the rest of the bar. Home is the landing page; widgets appended to it go below the
        // clock.
        void add_page(const std::string& name, GtkWidget* page);
        void add_to_home(GtkWidget* widget);
        bool has_page(const std::string& page) const;
        GtkWidget* page(const std::string& page) const; // null when there is none
        void navigate(const std::string& page);
        bool expanded() const { return expanded_; }
        // Runs each time the island opens, so pages can refresh what nothing signals (brightness).
        void on_open(std::function<void()> listener) { open_listeners_.push_back(std::move(listener)); }
        void on_close(std::function<void()> listener) { close_listeners_.push_back(std::move(listener)); }
        GtkWindow* window() const { return window_; }
        // Collapsed pill width, and a listener for when it changes, so the bar can keep clear of it.
        int pill_width() const { return pill_width_; }
        void on_pill_resize(std::function<void()> listener) { pill_listeners_.push_back(std::move(listener)); }

        // A back-to-home button and a title, for the top of a page.
        GtkWidget* page_header(const char* title);

        // Live activities in the collapsed pill, each falling back to the clock on its own. A level (volume,
        // brightness) outranks media while it shows.
        void show_osd(const char* icon, int percent);
        void show_media(const std::string& text, GdkPaintable* art);
        void set_media_art(GdkPaintable* art);
        // A passing event (charger plugged in) for a few seconds.
        void show_event(const char* icon, const std::string& text);
        // A plugin's passing notice: like an event, but yields to one. `image` wins over `icon` when set.
        void show_notice(const char* icon, GdkPaintable* image, const std::string& text);
        // Stays under everything timed until dismissed or the island is opened (battery low).
        void show_alert(const char* icon, const std::string& text);
        void dismiss_alert();

        // Home page sections, in order: header, tiles, controls, footer.
        void add_tile(GtkWidget* tile);
        void add_to_footer(GtkWidget* widget);
        // Pinned to the footer's right end, after everything added with add_to_footer.
        void add_to_footer_end(GtkWidget* widget);

        // Widget vfuncs, forwarded from the GtkWidget subclass in island.cpp.
        void measure(GtkOrientation orientation, int* minimum, int* natural) const;
        void allocate(int width, int height);
        void snapshot(GtkWidget* widget, GtkSnapshot* snapshot);
        void dispose();

    private:
        void build_pages();
        void show_page(const std::string& page, bool animate);
        void measure_page();
        void set_activity(const Activity& activity, guint ms);
        void retarget();
        void set_target(double width, double height);
        void grow_surface(int width, int height);
        void resize_surface(int width, int height);
        void update_input_region();
        void update_clock();
        void kick();

        double open_progress() const;

        static gboolean on_tick(GtkWidget* widget, GdkFrameClock* clock, gpointer data);
        static gboolean on_clock(gpointer data);
        static gboolean on_activity_done(gpointer data);
        static gboolean on_remeasure(gpointer data);
        void update_layer();
        static void on_pressed(GtkGestureClick* gesture, int n_press, double x, double y, gpointer data);
        static void on_enter(GtkEventControllerMotion* motion, double x, double y, gpointer data);
        static void on_motion(GtkEventControllerMotion* motion, double x, double y, gpointer data);
        static void on_leave(GtkEventControllerMotion* motion, gpointer data);
        static gboolean
            on_key(GtkEventControllerKey* key, guint keyval, guint code, GdkModifierType mods, gpointer data);
        static void on_active(GObject* window, GParamSpec* pspec, gpointer data);

        GtkApplication* app_ = nullptr;
        GtkWindow* window_ = nullptr;
        GtkWidget* root_ = nullptr; // the custom widget
        GtkWidget* shape_ = nullptr;
        GtkWidget* pill_ = nullptr;
        GtkWidget* pill_stack_ = nullptr;
        GtkWidget* pill_label_ = nullptr;
        GtkWidget* osd_icon_ = nullptr;
        GtkWidget* osd_level_ = nullptr;
        GtkWidget* media_art_ = nullptr;
        GtkWidget* media_label_ = nullptr;
        GtkWidget* event_icon_ = nullptr;
        GtkWidget* event_label_ = nullptr;
        GtkWidget* alert_icon_ = nullptr;
        GtkWidget* alert_label_ = nullptr;
        GtkWidget* home_box_ = nullptr;
        GtkWidget* tiles_ = nullptr;
        GtkWidget* footer_ = nullptr;
        GtkWidget* footer_end_ = nullptr;
        GtkWidget* stack_ = nullptr;
        GtkWidget* home_time_ = nullptr;
        GtkWidget* home_date_ = nullptr;
        GtkWidget* calendar_ = nullptr;
        GdkMonitor* monitor_ = nullptr;

        Spring width_;
        Spring height_;
        std::string page_ = "home";
        bool expanded_ = false;
        bool hovered_ = false;
        bool armed_ = false; // the pointer has moved inside the settled, open island
        bool had_focus_ = false;
        int surface_width_ = 0;
        int surface_height_ = 0;
        int pill_width_ = 0; // collapsed and not hovered
        int page_width_ = 0;
        int page_height_ = 0;
        cairo_rectangle_int_t input_ = {};
        guint tick_id_ = 0;
        gint64 last_frame_us_ = 0;
        guint clock_id_ = 0;
        guint activity_id_ = 0;
        ActivityQueue activities_;
        guint remeasure_id_ = 0;
        std::vector<std::function<void()>> open_listeners_;
        std::vector<std::function<void()>> close_listeners_;
        std::vector<std::function<void()>> pill_listeners_;
    };

} // namespace fenriz::bar
