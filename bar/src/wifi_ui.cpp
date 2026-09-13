#include "wifi_ui.hpp"

namespace fenriz::bar {

    namespace {

        constexpr guint RESCAN_SECONDS = 10; // NetworkManager ignores anything more eager

        std::string status(const WifiNetwork& n) {
            if (n.connecting)
                return "Connecting…";
            if (n.active)
                return "Connected";
            if (n.security == WifiSecurity::Unsupported)
                return "Enterprise or WEP: use nm-connection-editor";
            if (n.saved)
                return "Saved";
            return "";
        }

        // Rebuilt only for what the rows show: signal changes inside a bar do not count.
        bool same_rows(const std::vector<WifiNetwork>& a, const std::vector<WifiNetwork>& b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
                if (a[i].ssid != b[i].ssid || signal_bars(a[i].strength) != signal_bars(b[i].strength) ||
                    a[i].security != b[i].security || a[i].saved != b[i].saved || a[i].active != b[i].active ||
                    a[i].connecting != b[i].connecting)
                    return false;
            return true;
        }

    } // namespace

    WifiUi::WifiUi(Island& island, Network& network) : island_(island), net_(network) {
        tile_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(tile_, "island-split-tile");
        tile_toggle_ = gtk_toggle_button_new();
        gtk_widget_add_css_class(tile_toggle_, "island-tile");
        gtk_widget_set_hexpand(tile_toggle_, TRUE);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        tile_icon_ = gtk_image_new_from_icon_name("network-wireless-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(tile_icon_), 20);
        tile_label_ = gtk_label_new("Wi-Fi");
        gtk_widget_add_css_class(tile_label_, "island-tile-label");
        gtk_label_set_ellipsize(GTK_LABEL(tile_label_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(tile_label_), 12);
        gtk_box_append(GTK_BOX(box), tile_icon_);
        gtk_box_append(GTK_BOX(box), tile_label_);
        gtk_button_set_child(GTK_BUTTON(tile_toggle_), box);
        g_signal_connect(tile_toggle_,
                         "toggled",
                         G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
                             auto* self = static_cast<WifiUi*>(data);
                             if (!self->updating_)
                                 self->net_.set_enabled(gtk_toggle_button_get_active(b));
                         }),
                         this);
        GtkWidget* more = gtk_button_new_from_icon_name("go-next-symbolic");
        gtk_widget_add_css_class(more, "island-tile-more");
        g_signal_connect_swapped(more, "clicked", G_CALLBACK(+[](Island* i) { i->navigate("wifi"); }), &island_);
        gtk_box_append(GTK_BOX(tile_), tile_toggle_);
        gtk_box_append(GTK_BOX(tile_), more);
        gtk_widget_set_visible(tile_, FALSE);
        island_.add_tile(tile_);

        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget* header = island_.page_header("Wi-Fi");
        power_ = gtk_switch_new();
        gtk_widget_set_hexpand(power_, TRUE);
        gtk_widget_set_halign(power_, GTK_ALIGN_END);
        gtk_widget_set_valign(power_, GTK_ALIGN_CENTER);
        g_signal_connect(power_,
                         "state-set",
                         G_CALLBACK(+[](GtkSwitch*, gboolean on, gpointer data) -> gboolean {
                             auto* self = static_cast<WifiUi*>(data);
                             if (!self->updating_)
                                 self->net_.set_enabled(on);
                             return FALSE;
                         }),
                         this);
        gtk_box_append(GTK_BOX(header), power_);
        gtk_box_append(GTK_BOX(page), header);
        status_ = gtk_label_new(nullptr);
        gtk_widget_add_css_class(status_, "island-empty");
        gtk_box_append(GTK_BOX(page), status_);

        // long lists scroll inside the island instead of growing it past the screen
        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 360);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
        list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_);
        gtk_box_append(GTK_BOX(page), scroller);
        island_.add_page("wifi", page);

        // Scans only while someone is looking at the list.
        g_signal_connect_swapped(page,
                                 "map",
                                 G_CALLBACK(+[](WifiUi* self) {
                                     self->net_.scan();
                                     self->scan_id_ = g_timeout_add_seconds(
                                         RESCAN_SECONDS,
                                         +[](gpointer data) -> gboolean {
                                             static_cast<Network*>(data)->scan();
                                             return G_SOURCE_CONTINUE;
                                         },
                                         &self->net_);
                                 }),
                                 this);
        g_signal_connect_swapped(page,
                                 "unmap",
                                 G_CALLBACK(+[](WifiUi* self) {
                                     if (self->scan_id_)
                                         g_source_remove(self->scan_id_);
                                     self->scan_id_ = 0;
                                     self->expand("");
                                 }),
                                 this);

        net_.subscribe([this] { update(); });
        net_.on_connected([this](const std::string& ssid) { island_.show_event(net_.icon(), "Connected to " + ssid); });
        net_.on_error([this](const std::string& message, const std::string& ssid) {
            island_.show_event("network-wireless-no-route-symbolic", message);
            if (!ssid.empty())
                expand(ssid); // ask again, in place
        });
    }

    WifiUi::~WifiUi() {
        if (scan_id_)
            g_source_remove(scan_id_);
    }

    void WifiUi::update() {
        const bool usable = net_.running() && net_.has_wifi();
        gtk_widget_set_visible(tile_, usable);
        const bool on = usable && net_.wifi_enabled();
        const WifiNetwork* active = net_.active();

        updating_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tile_toggle_), on);
        gtk_switch_set_active(GTK_SWITCH(power_), on);
        gtk_widget_set_sensitive(power_, usable);
        updating_ = false;
        gtk_label_set_text(GTK_LABEL(tile_label_), active ? active->ssid.c_str() : "Wi-Fi");
        gtk_image_set_from_icon_name(GTK_IMAGE(tile_icon_),
                                     usable ? net_.icon() : "network-wireless-disabled-symbolic");

        const char* message = !net_.running()    ? "NetworkManager is not running"
                              : !net_.has_wifi() ? "No Wi-Fi device"
                              : !on              ? "Wi-Fi is off"
                                                 : nullptr;
        gtk_widget_set_visible(status_, message != nullptr);
        if (message)
            gtk_label_set_text(GTK_LABEL(status_), message);
        gtk_widget_set_visible(list_, on);

        if (!same_rows(net_.networks(), shown_))
            rebuild();
    }

    void WifiUi::rebuild() {
        shown_ = net_.networks();
        entry_ = nullptr;
        while (GtkWidget* child = gtk_widget_get_first_child(list_))
            gtk_box_remove(GTK_BOX(list_), child);
        for (const WifiNetwork& n : shown_)
            gtk_box_append(GTK_BOX(list_), row(n));
        if (entry_)
            gtk_widget_grab_focus(entry_); // only once it is in the window
    }

    GtkWidget* WifiUi::row(const WifiNetwork& n) {
        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(outer, "island-device-row");
        if (n.active)
            gtk_widget_add_css_class(outer, "connected");

        GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget* button = gtk_button_new();
        gtk_widget_add_css_class(button, "island-device-button");
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_set_sensitive(button, n.security != WifiSecurity::Unsupported && !n.connecting);
        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name(signal_icon(n.strength)));
        GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* name = gtk_label_new(n.ssid.c_str());
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 26);
        gtk_box_append(GTK_BOX(text), name);
        const std::string state = status(n);
        if (!state.empty()) {
            GtkWidget* sub = gtk_label_new(state.c_str());
            gtk_widget_add_css_class(sub, "island-track-artist");
            gtk_label_set_xalign(GTK_LABEL(sub), 0);
            gtk_box_append(GTK_BOX(text), sub);
        }
        gtk_widget_set_hexpand(text, TRUE);
        gtk_box_append(GTK_BOX(content), text);
        if (n.security != WifiSecurity::Open) {
            GtkWidget* lock = gtk_image_new_from_icon_name("channel-secure-symbolic");
            gtk_widget_add_css_class(lock, "island-dim");
            gtk_box_append(GTK_BOX(content), lock);
        }
        gtk_button_set_child(GTK_BUTTON(button), content);
        g_object_set_data_full(G_OBJECT(button), "ssid", g_strdup(n.ssid.c_str()), g_free);
        g_signal_connect(button,
                         "clicked",
                         G_CALLBACK(+[](GtkButton* b, gpointer data) {
                             auto* self = static_cast<WifiUi*>(data);
                             const std::string ssid = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "ssid"));
                             for (const WifiNetwork& n : self->net_.networks()) {
                                 if (n.ssid != ssid)
                                     continue;
                                 if (n.active)
                                     self->net_.disconnect();
                                 else if (n.saved || n.security == WifiSecurity::Open)
                                     self->net_.connect(ssid, "");
                                 else
                                     self->expand(self->expanded_ == ssid ? "" : ssid);
                                 return;
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(line), button);

        if (n.saved) {
            GtkWidget* forget = gtk_button_new_from_icon_name("user-trash-symbolic");
            gtk_widget_add_css_class(forget, "island-forget");
            gtk_widget_set_tooltip_text(forget, "Forget");
            gtk_widget_set_valign(forget, GTK_ALIGN_CENTER);
            g_object_set_data_full(G_OBJECT(forget), "ssid", g_strdup(n.ssid.c_str()), g_free);
            g_signal_connect(forget,
                             "clicked",
                             G_CALLBACK(+[](GtkButton* b, gpointer data) {
                                 static_cast<Network*>(data)->forget(
                                     static_cast<const char*>(g_object_get_data(G_OBJECT(b), "ssid")));
                             }),
                             &net_);
            gtk_box_append(GTK_BOX(line), forget);
        }
        gtk_box_append(GTK_BOX(outer), line);

        if (n.ssid == expanded_) {
            GtkWidget* ask = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_add_css_class(ask, "island-password");
            entry_ = gtk_password_entry_new();
            gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry_), TRUE);
            gtk_widget_set_hexpand(entry_, TRUE);
            gtk_widget_add_css_class(entry_, "fenriz-field");
            gtk_editable_set_text(GTK_EDITABLE(entry_), password_.c_str());
            g_signal_connect_swapped(entry_, "activate", G_CALLBACK(+[](WifiUi* self) { self->submit(); }), this);
            g_signal_connect(entry_,
                             "changed",
                             G_CALLBACK(+[](GtkEditable* e, gpointer data) {
                                 static_cast<WifiUi*>(data)->password_ = gtk_editable_get_text(e);
                             }),
                             this);
            GtkWidget* join = gtk_button_new_with_label("Connect");
            gtk_widget_add_css_class(join, "island-join");
            g_signal_connect_swapped(join, "clicked", G_CALLBACK(+[](WifiUi* self) { self->submit(); }), this);
            gtk_box_append(GTK_BOX(ask), entry_);
            gtk_box_append(GTK_BOX(ask), join);
            gtk_box_append(GTK_BOX(outer), ask);
        }
        return outer;
    }

    void WifiUi::expand(const std::string& ssid) {
        if (ssid == expanded_)
            return;
        expanded_ = ssid;
        password_.clear();
        rebuild();
    }

    void WifiUi::submit() {
        if (expanded_.empty() || password_.empty())
            return;
        const std::string ssid = expanded_, password = password_;
        expanded_.clear();
        password_.clear();
        rebuild();
        net_.connect(ssid, password);
    }

} // namespace fenriz::bar
