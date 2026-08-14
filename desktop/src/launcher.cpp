#include "launcher.hpp"

#include <gio/gdesktopappinfo.h>
#include <gtk4-layer-shell.h>

#include <algorithm>

#include "blur.hpp"
#include "spawn.hpp"
#include "theme.hpp"

namespace fenriz::desktop {

    namespace {
        constexpr int WIDTH = 620;
        constexpr int MAX_LIST_HEIGHT = 420;
        constexpr int TOP_MARGIN = 160;

        constexpr size_t EMPTY_ROWS = 5;
        constexpr size_t MAX_ROWS = 20;

        std::string str_or_empty(const char* s) { return s ? std::string(s) : std::string(); }
    } // namespace

    Launcher::Launcher(const Config& cfg) : cfg_(cfg) { usage_.load(); }

    Launcher::~Launcher() {
        if (app_monitor_) {
            g_signal_handlers_disconnect_by_data(app_monitor_, this);
            g_object_unref(app_monitor_);
        }
        for (Entry& e : entries_)
            if (e.info)
                g_object_unref(e.info);
        if (window_)
            gtk_window_destroy(window_);
    }

    void Launcher::load_entries() {
        for (Entry& e : entries_)
            if (e.info)
                g_object_unref(e.info);
        entries_.clear();

        GList* all = g_app_info_get_all();
        for (GList* l = all; l; l = l->next) {
            auto* info = static_cast<GAppInfo*>(l->data);
            // should_show honours NoDisplay, Hidden, and OnlyShowIn/NotShowIn.
            if (!g_app_info_should_show(info))
                continue;
            const char* id = g_app_info_get_id(info);
            const char* name = g_app_info_get_display_name(info);
            if (!id || !name)
                continue;

            MatchFields f;
            f.name = name;
            f.comment = str_or_empty(g_app_info_get_description(info));
            f.exec = str_or_empty(g_app_info_get_commandline(info));
            if (G_IS_DESKTOP_APP_INFO(info)) {
                auto* dinfo = G_DESKTOP_APP_INFO(info);
                f.generic_name = str_or_empty(g_desktop_app_info_get_generic_name(dinfo));
                // Borrowed, NULL-terminated, owned by the info.
                for (const char* const* k = g_desktop_app_info_get_keywords(dinfo); k && *k; k++)
                    f.keywords.emplace_back(*k);
            }
            entries_.push_back(Entry{G_APP_INFO(g_object_ref(info)), id, std::move(f)});
        }
        g_list_free_full(all, g_object_unref);
        entries_stale_ = false;
    }

    void Launcher::on_apps_changed(GAppInfoMonitor*, gpointer data) {
        static_cast<Launcher*>(data)->entries_stale_ = true;
    }

    void Launcher::refilter() {
        const char* raw = gtk_editable_get_text(GTK_EDITABLE(search_));
        const std::string query = raw ? raw : "";
        const int64_t now = now_ms();

        // An empty query scores everything alike, leaving frecency to order the whole list.
        std::vector<int> scores(entries_.size(), 0);
        shown_.clear();
        for (size_t i = 0; i < entries_.size(); i++) {
            if (!query.empty()) {
                const int s = match_score(entries_[i].fields, query);
                if (s < 0)
                    continue;
                scores[i] = s;
            }
            shown_.push_back(static_cast<int>(i));
        }

        std::stable_sort(shown_.begin(), shown_.end(), [&](int a, int b) {
            if (scores[a] != scores[b])
                return scores[a] > scores[b];
            const double fa = usage_.score_of(entries_[a].id, now);
            const double fb = usage_.score_of(entries_[b].id, now);
            if (fa != fb)
                return fa > fb;
            return g_utf8_collate(entries_[a].fields.name.c_str(), entries_[b].fields.name.c_str()) < 0;
        });

        const size_t cap = query.empty() ? EMPTY_ROWS : MAX_ROWS;
        if (shown_.size() > cap)
            shown_.resize(cap);

        gtk_list_box_remove_all(GTK_LIST_BOX(list_));
        for (int idx : shown_) {
            GtkWidget* row = gtk_list_box_row_new();
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_widget_set_margin_start(box, 10);
            gtk_widget_set_margin_end(box, 10);
            gtk_widget_set_margin_top(box, 6);
            gtk_widget_set_margin_bottom(box, 6);

            GtkWidget* icon = gtk_image_new_from_gicon(g_app_info_get_icon(entries_[idx].info));
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 28);
            gtk_box_append(GTK_BOX(box), icon);

            GtkWidget* label = gtk_label_new(entries_[idx].fields.name.c_str());
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(box), label);

            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
            g_object_set_data(G_OBJECT(row), "entry-index", GINT_TO_POINTER(idx));
            gtk_list_box_append(GTK_LIST_BOX(list_), row);
        }

        if (GtkListBoxRow* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), 0))
            gtk_list_box_select_row(GTK_LIST_BOX(list_), first);
    }

    void Launcher::activate_row(GtkListBoxRow* row) {
        if (!row)
            return;
        const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "entry-index"));
        if (idx < 0 || idx >= static_cast<int>(entries_.size()))
            return;
        Entry& e = entries_[idx];

        if (!spawn::app(e.info))
            return; // a failed launch must not count as a use
        usage_.record(e.id, now_ms());
        close();
    }

    void Launcher::move_selection(int delta) {
        GtkListBox* box = GTK_LIST_BOX(list_);
        GtkListBoxRow* current = gtk_list_box_get_selected_row(box);
        int index = current ? gtk_list_box_row_get_index(current) : -1;
        const int count = static_cast<int>(shown_.size());
        if (count == 0)
            return;
        index = std::clamp(index + delta, 0, count - 1);
        if (GtkListBoxRow* next = gtk_list_box_get_row_at_index(box, index)) {
            gtk_list_box_select_row(box, next);
            gtk_widget_grab_focus(GTK_WIDGET(next));
            gtk_widget_grab_focus(search_); // keep typing in the entry
        }
    }

    void Launcher::on_search_changed(GtkEditable*, gpointer data) { static_cast<Launcher*>(data)->refilter(); }

    void Launcher::on_row_activated(GtkListBox*, GtkListBoxRow* row, gpointer data) {
        static_cast<Launcher*>(data)->activate_row(row);
    }

    gboolean Launcher::on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
        auto* self = static_cast<Launcher*>(data);
        switch (keyval) {
        case GDK_KEY_Escape:
            self->close();
            return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            self->activate_row(gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_)));
            return TRUE;
        case GDK_KEY_Down:
            self->move_selection(+1);
            return TRUE;
        case GDK_KEY_Up:
            self->move_selection(-1);
            return TRUE;
        default:
            return FALSE;
        }
    }

    void Launcher::build(GtkApplication* app) {
        window_ = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(window_);
        gtk_layer_set_namespace(window_, "fenriz-launcher");
        gtk_layer_set_layer(window_, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_window_set_default_size(window_, WIDTH, -1);
        gtk_layer_set_anchor(window_, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_margin(window_, GTK_LAYER_SHELL_EDGE_TOP, TOP_MARGIN);
        gtk_widget_add_css_class(GTK_WIDGET(window_), "fenriz-shell");

        app_monitor_ = g_app_info_monitor_get();
        g_signal_connect(app_monitor_, "changed", G_CALLBACK(on_apps_changed), this);

        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(root, "fenriz-launcher");

        search_ = gtk_search_entry_new();
        gtk_widget_add_css_class(search_, "fenriz-field");
        gtk_widget_set_margin_start(search_, 10);
        gtk_widget_set_margin_end(search_, 10);
        gtk_widget_set_margin_top(search_, 10);
        gtk_widget_set_margin_bottom(search_, 6);
        g_signal_connect(search_, "changed", G_CALLBACK(on_search_changed), this);
        gtk_box_append(GTK_BOX(root), search_);

        list_ = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
        g_signal_connect(list_, "row-activated", G_CALLBACK(on_row_activated), this);

        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), MAX_LIST_HEIGHT);
        gtk_box_append(GTK_BOX(root), scroll);

        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), this);
        gtk_widget_add_controller(GTK_WIDGET(window_), keys);

        gtk_window_set_child(window_, root);
        gtk_window_set_focus(window_, search_);
        blur::attach(GTK_NATIVE(window_), root, blur::Mode::Widget, theme::CARD_RADIUS);
    }

    void Launcher::prewarm(GtkApplication* app) {
        if (window_)
            return;
        build(app);
        load_entries();
        gtk_widget_realize(GTK_WIDGET(window_));
    }

    void Launcher::toggle(GtkApplication* app) {
        if (window_ && gtk_widget_get_visible(GTK_WIDGET(window_))) {
            close();
            return;
        }
        if (!window_)
            build(app);
        if (entries_stale_)
            load_entries();
        usage_.load();
        gtk_editable_set_text(GTK_EDITABLE(search_), "");
        refilter();
        gtk_window_present(window_);
        gtk_widget_grab_focus(search_);
    }

    void Launcher::close() {
        if (window_)
            gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }

} // namespace fenriz::desktop
