#include "system_ui.hpp"

#include <cmath>

namespace fenriz::bar {

    namespace {

        // A filled line of the last samples, 0..100 bottom to top, in the widget's own color.
        void draw_sparkline(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
            const auto* history = static_cast<const std::deque<int>*>(data);
            if (history->size() < 2)
                return;
            GdkRGBA color;
            gtk_widget_get_color(GTK_WIDGET(area), &color);
            const double step = static_cast<double>(width) / (SysStat::HISTORY - 1);
            // newest sample on the right edge
            const double x0 = width - step * (history->size() - 1);
            auto y = [height](int v) { return height - 1 - (height - 2) * v / 100.0; };

            cairo_move_to(cr, x0, y(history->front()));
            for (size_t i = 1; i < history->size(); i++)
                cairo_line_to(cr, x0 + step * i, y((*history)[i]));
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
            cairo_stroke_preserve(cr);
            cairo_line_to(cr, width, height);
            cairo_line_to(cr, x0, height);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha * 0.18);
            cairo_fill(cr);
        }

        GtkWidget* sparkline(const std::deque<int>& history) {
            GtkWidget* area = gtk_drawing_area_new();
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 36);
            gtk_widget_set_hexpand(area, TRUE);
            gtk_widget_add_css_class(area, "island-sparkline");
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(area), draw_sparkline, const_cast<std::deque<int>*>(&history), nullptr);
            return area;
        }

        GtkWidget* stat_row(GtkWidget* grid, int row, const char* name, GtkWidget** value) {
            GtkWidget* label = gtk_label_new(name);
            gtk_widget_add_css_class(label, "island-section");
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            *value = gtk_label_new("–");
            gtk_widget_add_css_class(*value, "island-stat");
            gtk_label_set_xalign(GTK_LABEL(*value), 1);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), *value, 1, row, 1, 1);
            return label;
        }

    } // namespace

    SystemUi::SystemUi(Island& island, SysStat& stats) : island_(island), stats_(stats) {
        readout_ = gtk_button_new_with_label("");
        gtk_widget_add_css_class(readout_, "island-flat");
        gtk_widget_add_css_class(readout_, "island-footer-text");
        g_signal_connect_swapped(readout_, "clicked", G_CALLBACK(+[](Island* i) { i->navigate("system"); }), &island_);
        island_.add_to_footer(readout_);

        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(page), island_.page_header("System"));
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        gtk_widget_add_css_class(grid, "island-stats");

        stat_row(grid, 0, "CPU", &cpu_);
        cpu_graph_ = sparkline(stats_.cpu_history());
        gtk_grid_attach(GTK_GRID(grid), cpu_graph_, 0, 1, 2, 1);
        stat_row(grid, 2, "Memory", &memory_);
        memory_graph_ = sparkline(stats_.memory_history());
        gtk_grid_attach(GTK_GRID(grid), memory_graph_, 0, 3, 2, 1);
        stat_row(grid, 4, "Disk", &disk_);
        disk_bar_ = gtk_level_bar_new_for_interval(0, 100);
        gtk_widget_add_css_class(disk_bar_, "island-level");
        gtk_grid_attach(GTK_GRID(grid), disk_bar_, 0, 5, 2, 1);
        stat_row(grid, 6, "Temperature", &temp_);
        gtk_box_append(GTK_BOX(page), grid);
        island_.add_page("system", page);

        // Sampling only happens while the island is open, so a closed bar costs no wakeups.
        island_.on_open([this] { stats_.watch(true); });
        island_.on_close([this] { stats_.watch(false); });
        stats_.subscribe([this] { update(); });
    }

    void SystemUi::update() {
        const SysStat::Sample& s = stats_.latest();
        auto percent = [](int v) { return v < 0 ? std::string("–") : std::to_string(v) + "%"; };
        const std::string temp = s.celsius < 0 ? "–" : std::to_string(static_cast<int>(std::lround(s.celsius))) + "°";

        std::string readout = "CPU " + percent(s.cpu) + "  ·  RAM " + percent(s.memory);
        if (s.celsius >= 0)
            readout += "  ·  " + temp;
        gtk_button_set_label(GTK_BUTTON(readout_), readout.c_str());

        gtk_label_set_text(GTK_LABEL(cpu_), percent(s.cpu).c_str());
        gtk_label_set_text(GTK_LABEL(memory_), percent(s.memory).c_str());
        gtk_label_set_text(GTK_LABEL(disk_), percent(s.disk).c_str());
        gtk_level_bar_set_value(GTK_LEVEL_BAR(disk_bar_), std::max(s.disk, 0));
        gtk_label_set_text(GTK_LABEL(temp_), temp.c_str());
        gtk_widget_queue_draw(cpu_graph_);
        gtk_widget_queue_draw(memory_graph_);
    }

} // namespace fenriz::bar
