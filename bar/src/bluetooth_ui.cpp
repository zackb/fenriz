#include "bluetooth_ui.hpp"

namespace fenriz::bar {

    namespace {

        GtkWidget* section_label(const char* text) {
            GtkWidget* l = gtk_label_new(text);
            gtk_widget_add_css_class(l, "island-section");
            gtk_widget_set_halign(l, GTK_ALIGN_START);
            return l;
        }

        // The device's own icon if the theme has it, else a generic one.
        GIcon* device_icon(const BtDevice& d) {
            const std::string name = bt_device_icon(d.icon);
            const char* names[] = {name.c_str(), "bluetooth-symbolic", nullptr};
            return g_themed_icon_new_from_names(const_cast<char**>(names), -1);
        }

        std::string status(const BtDevice& d) {
            if (d.busy)
                return d.connected ? "Disconnecting…" : d.paired ? "Connecting…" : "Pairing…";
            if (!d.paired)
                return "";
            std::string s = d.connected ? "Connected" : "Not connected";
            if (d.connected && d.battery >= 0)
                s += " · " + std::to_string(d.battery) + "%";
            return s;
        }

        // What the rows show, so RSSI churn during a scan does not rebuild them when their order holds.
        bool same_rows(const std::vector<BtDevice>& a, const std::vector<BtDevice>& b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
                if (a[i].path != b[i].path || a[i].name != b[i].name || a[i].icon != b[i].icon ||
                    a[i].paired != b[i].paired || a[i].connected != b[i].connected || a[i].battery != b[i].battery ||
                    a[i].busy != b[i].busy)
                    return false;
            return true;
        }

    } // namespace

    BluetoothUi::BluetoothUi(Island& island, Bluetooth& bluetooth) : island_(island), bt_(bluetooth) {
        // home tile: the body toggles the radio, the arrow opens the page
        tile_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(tile_, "island-split-tile");
        tile_toggle_ = gtk_toggle_button_new();
        gtk_widget_add_css_class(tile_toggle_, "island-tile");
        gtk_widget_set_hexpand(tile_toggle_, TRUE);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        tile_icon_ = gtk_image_new_from_icon_name("bluetooth-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(tile_icon_), 20);
        tile_label_ = gtk_label_new("Bluetooth");
        gtk_widget_add_css_class(tile_label_, "island-tile-label");
        gtk_label_set_ellipsize(GTK_LABEL(tile_label_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(tile_label_), 12);
        gtk_box_append(GTK_BOX(box), tile_icon_);
        gtk_box_append(GTK_BOX(box), tile_label_);
        gtk_button_set_child(GTK_BUTTON(tile_toggle_), box);
        g_signal_connect(tile_toggle_,
                         "toggled",
                         G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
                             auto* self = static_cast<BluetoothUi*>(data);
                             if (!self->updating_)
                                 self->bt_.set_powered(gtk_toggle_button_get_active(b));
                         }),
                         this);
        GtkWidget* more = gtk_button_new_from_icon_name("go-next-symbolic");
        gtk_widget_add_css_class(more, "island-tile-more");
        g_signal_connect_swapped(more, "clicked", G_CALLBACK(+[](Island* i) { i->navigate("bluetooth"); }), &island_);
        gtk_box_append(GTK_BOX(tile_), tile_toggle_);
        gtk_box_append(GTK_BOX(tile_), more);
        gtk_widget_set_visible(tile_, FALSE);
        island_.add_tile(tile_);

        // the page
        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget* header = island_.page_header("Bluetooth");
        power_ = gtk_switch_new();
        gtk_widget_set_hexpand(power_, TRUE);
        gtk_widget_set_halign(power_, GTK_ALIGN_END);
        gtk_widget_set_valign(power_, GTK_ALIGN_CENTER);
        g_signal_connect(power_,
                         "state-set",
                         G_CALLBACK(+[](GtkSwitch*, gboolean on, gpointer data) -> gboolean {
                             auto* self = static_cast<BluetoothUi*>(data);
                             if (!self->updating_)
                                 self->bt_.set_powered(on);
                             return FALSE;
                         }),
                         this);
        gtk_box_append(GTK_BOX(header), power_);
        gtk_box_append(GTK_BOX(page), header);

        off_label_ = gtk_label_new("Bluetooth is off");
        gtk_widget_add_css_class(off_label_, "island-empty");
        gtk_box_append(GTK_BOX(page), off_label_);

        paired_section_ = section_label("Devices");
        paired_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(page), paired_section_);
        gtk_box_append(GTK_BOX(page), paired_);

        GtkWidget* nearby_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(nearby_header), section_label("Nearby"));
        spinner_ = gtk_spinner_new();
        gtk_box_append(GTK_BOX(nearby_header), spinner_);
        nearby_section_ = nearby_header;
        nearby_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(page), nearby_section_);
        gtk_box_append(GTK_BOX(page), nearby_);
        searching_ = gtk_label_new("Searching…");
        gtk_widget_add_css_class(searching_, "island-empty");
        gtk_box_append(GTK_BOX(page), searching_);
        island_.add_page("bluetooth", page);

        // Scanning costs radio time (and audio quality on a connected headset), so only while the page shows.
        g_signal_connect_swapped(page, "map", G_CALLBACK(+[](Bluetooth* bt) { bt->discover(true); }), &bt_);
        g_signal_connect_swapped(page, "unmap", G_CALLBACK(+[](Bluetooth* bt) { bt->discover(false); }), &bt_);

        bt_.subscribe([this] { update(); });
        bt_.on_connection([this](const BtDevice& d, bool connected) {
            const std::string text = d.name + (connected ? " connected" : " disconnected") +
                                     (connected && d.battery >= 0 ? " · " + std::to_string(d.battery) + "%" : "");
            island_.show_event(bt_device_icon(d.icon).c_str(), text);
        });
        bt_.on_error([this](const std::string& message) { island_.show_event("dialog-warning-symbolic", message); });
    }

    void BluetoothUi::update() {
        gtk_widget_set_visible(tile_, bt_.available());
        if (!bt_.available())
            return;

        const bool on = bt_.powered();
        std::vector<BtDevice> paired, nearby;
        const BtDevice* connected = nullptr;
        for (const BtDevice& d : bt_.devices()) {
            if (d.paired)
                paired.push_back(d);
            else if (d.has_name) // a bare address is noise nobody can pick from
                nearby.push_back(d);
        }
        for (const BtDevice& d : paired)
            if (d.connected) {
                connected = &d;
                break;
            }

        updating_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tile_toggle_), on);
        gtk_switch_set_active(GTK_SWITCH(power_), on);
        updating_ = false;
        gtk_label_set_text(GTK_LABEL(tile_label_), connected ? connected->name.c_str() : "Bluetooth");
        gtk_image_set_from_icon_name(GTK_IMAGE(tile_icon_),
                                     !on         ? "bluetooth-disabled-symbolic"
                                     : connected ? "bluetooth-active-symbolic"
                                                 : "bluetooth-symbolic");

        gtk_widget_set_visible(off_label_, !on);
        gtk_widget_set_visible(paired_section_, on && !paired.empty());
        gtk_widget_set_visible(paired_, on);
        gtk_widget_set_visible(nearby_section_, on);
        gtk_widget_set_visible(nearby_, on);
        gtk_spinner_set_spinning(GTK_SPINNER(spinner_), bt_.discovering());
        gtk_widget_set_visible(spinner_, bt_.discovering());
        gtk_widget_set_visible(searching_, on && bt_.discovering() && nearby.empty());
        rebuild(paired_, paired, paired_shown_);
        rebuild(nearby_, nearby, nearby_shown_);
    }

    void BluetoothUi::rebuild(GtkWidget* list, const std::vector<BtDevice>& devices, std::vector<BtDevice>& shown) {
        if (same_rows(devices, shown))
            return;
        shown = devices;
        while (GtkWidget* child = gtk_widget_get_first_child(list))
            gtk_box_remove(GTK_BOX(list), child);
        for (const BtDevice& d : devices)
            gtk_box_append(GTK_BOX(list), row(d));
    }

    GtkWidget* BluetoothUi::row(const BtDevice& d) {
        GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(line, "island-device-row");
        if (d.connected)
            gtk_widget_add_css_class(line, "connected");

        GtkWidget* button = gtk_button_new();
        gtk_widget_add_css_class(button, "island-device-button");
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_set_sensitive(button, !d.busy);
        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GIcon* icon = device_icon(d);
        GtkWidget* image = gtk_image_new_from_gicon(icon);
        g_object_unref(icon);
        gtk_box_append(GTK_BOX(content), image);
        GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* name = gtk_label_new(d.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 28);
        gtk_box_append(GTK_BOX(text), name);
        const std::string state = status(d);
        if (!state.empty()) {
            GtkWidget* sub = gtk_label_new(state.c_str());
            gtk_widget_add_css_class(sub, "island-track-artist");
            gtk_label_set_xalign(GTK_LABEL(sub), 0);
            gtk_box_append(GTK_BOX(text), sub);
        }
        gtk_box_append(GTK_BOX(content), text);
        gtk_button_set_child(GTK_BUTTON(button), content);

        g_object_set_data_full(G_OBJECT(button), "path", g_strdup(d.path.c_str()), g_free);
        g_object_set_data(G_OBJECT(button), "paired", GINT_TO_POINTER(d.paired));
        g_object_set_data(G_OBJECT(button), "connected", GINT_TO_POINTER(d.connected));
        g_signal_connect(button,
                         "clicked",
                         G_CALLBACK(+[](GtkButton* b, gpointer data) {
                             auto* bt = static_cast<Bluetooth*>(data);
                             const std::string path = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "path"));
                             if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "connected")))
                                 bt->disconnect(path);
                             else if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "paired")))
                                 bt->connect(path);
                             else
                                 bt->pair(path);
                         }),
                         &bt_);
        gtk_box_append(GTK_BOX(line), button);

        if (d.paired) {
            GtkWidget* forget = gtk_button_new_from_icon_name("user-trash-symbolic");
            gtk_widget_add_css_class(forget, "island-forget");
            gtk_widget_set_tooltip_text(forget, "Forget");
            gtk_widget_set_valign(forget, GTK_ALIGN_CENTER);
            g_object_set_data_full(G_OBJECT(forget), "path", g_strdup(d.path.c_str()), g_free);
            g_signal_connect(forget,
                             "clicked",
                             G_CALLBACK(+[](GtkButton* b, gpointer data) {
                                 static_cast<Bluetooth*>(data)->forget(
                                     static_cast<const char*>(g_object_get_data(G_OBJECT(b), "path")));
                             }),
                             &bt_);
            gtk_box_append(GTK_BOX(line), forget);
        }
        return line;
    }

} // namespace fenriz::bar
