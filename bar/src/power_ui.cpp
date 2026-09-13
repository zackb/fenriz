#include "power_ui.hpp"

#include <algorithm>
#include <cmath>

namespace fenriz::bar {

    namespace {

        constexpr guint CONFIRM_MS = 3000;

        const char* profile_icon(const std::string& profile) {
            if (profile == "power-saver")
                return "fenriz-leaf-symbolic";
            if (profile == "performance")
                return "fenriz-lightning-bolt-symbolic";
            return "fenriz-scale-balance-symbolic";
        }

        const char* profile_label(const std::string& profile) {
            if (profile == "power-saver")
                return "Saver";
            if (profile == "performance")
                return "Performance";
            return "Balanced";
        }

        const char* awake_icon(bool on) { return on ? "fenriz-eye-symbolic" : "fenriz-eye-off-symbolic"; }

        std::string battery_text(const Power::Battery& b) {
            std::string text = std::to_string(static_cast<int>(std::lround(b.percent))) + "%";
            const std::string left = format_duration(b.seconds_left);
            if (b.charge == Charge::Full)
                text += " · Full";
            else if (b.charge == Charge::Charging)
                text += left.empty() ? " · Charging" : " · " + left + " to full";
            else if (!left.empty())
                text += " · " + left + " left";
            return text;
        }

        // A tile: icon over a label, toggle or plain.
        GtkWidget*
            tile(GtkWidget* button, const char* icon, const char* text, GtkWidget** icon_out, GtkWidget** label_out) {
            gtk_widget_add_css_class(button, "island-tile");
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            GtkWidget* image = icon ? gtk_image_new_from_icon_name(icon) : gtk_image_new();
            gtk_image_set_pixel_size(GTK_IMAGE(image), 20);
            GtkWidget* label = gtk_label_new(text);
            gtk_widget_add_css_class(label, "island-tile-label");
            gtk_box_append(GTK_BOX(box), image);
            gtk_box_append(GTK_BOX(box), label);
            gtk_button_set_child(GTK_BUTTON(button), box);
            if (icon_out)
                *icon_out = image;
            if (label_out)
                *label_out = label;
            return button;
        }

    } // namespace

    PowerUi::PowerUi(Island& island, Power& power, Compositor& compositor, IdleInhibitor& inhibitor)
        : island_(island), power_(power), compositor_(compositor), inhibitor_(inhibitor) {
        // tiles
        if (inhibitor_.available()) {
            GtkWidget* image = nullptr;
            awake_tile_ = tile(gtk_toggle_button_new(), nullptr, "Keep awake", &image, nullptr);
            gtk_image_set_from_icon_name(GTK_IMAGE(image), awake_icon(true));
            g_signal_connect(awake_tile_,
                             "toggled",
                             G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
                                 static_cast<PowerUi*>(data)->inhibitor_.set(gtk_toggle_button_get_active(b));
                             }),
                             this);
            island_.add_tile(awake_tile_);
        }
        mode_tile_ = tile(gtk_button_new(), profile_icon("balanced"), "Balanced", &mode_icon_, &mode_label_);
        g_signal_connect_swapped(mode_tile_,
                                 "clicked",
                                 G_CALLBACK(+[](PowerUi* self) {
                                     // cycles through the modes in the daemon's order
                                     const auto& all = self->power_.profiles();
                                     auto it = std::find(all.begin(), all.end(), self->power_.profile());
                                     if (all.empty())
                                         return;
                                     const size_t next = it == all.end() ? 0 : (it - all.begin() + 1) % all.size();
                                     self->power_.set_profile(all[next]);
                                 }),
                                 this);
        gtk_widget_set_visible(mode_tile_, FALSE);
        island_.add_tile(mode_tile_);

        // footer, after the system readout: spacer, battery, power
        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        island_.add_to_footer(spacer);

        battery_footer_ = gtk_button_new();
        gtk_widget_add_css_class(battery_footer_, "island-flat");
        GtkWidget* battery_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        battery_footer_icon_ = gtk_image_new();
        battery_footer_label_ = gtk_label_new(nullptr);
        gtk_widget_add_css_class(battery_footer_label_, "island-footer-text");
        gtk_box_append(GTK_BOX(battery_box), battery_footer_icon_);
        gtk_box_append(GTK_BOX(battery_box), battery_footer_label_);
        gtk_button_set_child(GTK_BUTTON(battery_footer_), battery_box);
        g_signal_connect_swapped(
            battery_footer_, "clicked", G_CALLBACK(+[](Island* i) { i->navigate("power"); }), &island_);
        gtk_widget_set_visible(battery_footer_, FALSE);
        island_.add_to_footer(battery_footer_);

        GtkWidget* power_button = gtk_button_new_from_icon_name("fenriz-power-symbolic");
        gtk_widget_add_css_class(power_button, "island-icon-button");
        g_signal_connect_swapped(
            power_button, "clicked", G_CALLBACK(+[](Island* i) { i->navigate("power"); }), &island_);
        island_.add_to_footer(power_button);

        // the Power page
        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(page), island_.page_header("Power"));
        battery_line_ = gtk_label_new(nullptr);
        gtk_widget_add_css_class(battery_line_, "island-battery");
        gtk_widget_set_halign(battery_line_, GTK_ALIGN_START);
        gtk_widget_set_visible(battery_line_, FALSE);
        gtk_box_append(GTK_BOX(page), battery_line_);
        profiles_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_set_homogeneous(GTK_BOX(profiles_), TRUE);
        gtk_widget_add_css_class(profiles_, "linked");
        gtk_widget_add_css_class(profiles_, "island-profiles");
        gtk_widget_set_visible(profiles_, FALSE);
        gtk_box_append(GTK_BOX(page), profiles_);

        actions_ = {
            {"Lock",
             "fenriz-lock-symbolic",
             nullptr,
             [](PowerUi&) {
                 logind("/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session", "Lock", nullptr);
             }},
            {"Sleep",
             "fenriz-sleep-symbolic",
             nullptr,
             [](PowerUi&) {
                 logind("/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "Suspend",
                        g_variant_new("(b)", TRUE));
             }},
            {"Log out",
             "fenriz-logout-symbolic",
             "Click again to log out",
             [](PowerUi& self) {
                 if (self.compositor_.connected())
                     self.compositor_.exit();
                 else
                     logind("/org/freedesktop/login1/session/auto",
                            "org.freedesktop.login1.Session",
                            "Terminate",
                            nullptr);
             }},
            {"Restart",
             "fenriz-restart-symbolic",
             "Click again to restart",
             [](PowerUi&) {
                 logind(
                     "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Reboot", g_variant_new("(b)", TRUE));
             }},
            {"Shut down",
             "fenriz-power-symbolic",
             "Click again to shut down",
             [](PowerUi&) {
                 logind("/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "PowerOff",
                        g_variant_new("(b)", TRUE));
             }},
        };
        GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(list, "island-actions");
        for (Action& a : actions_) {
            a.button = gtk_button_new();
            gtk_widget_add_css_class(a.button, "island-action");
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(a.icon));
            a.text = gtk_label_new(a.label);
            gtk_box_append(GTK_BOX(row), a.text);
            gtk_button_set_child(GTK_BUTTON(a.button), row);
            g_object_set_data(G_OBJECT(a.button), "action", &a);
            g_signal_connect(a.button, "clicked", G_CALLBACK(on_action), this);
            gtk_box_append(GTK_BOX(list), a.button);
        }
        gtk_box_append(GTK_BOX(page), list);
        island_.add_page("power", page);

        power_.subscribe([this] { update(); });
    }

    bool PowerUi::toggle_awake() {
        if (!awake_tile_)
            return false;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(awake_tile_), !inhibitor_.active()); // "toggled" applies it
        island_.show_event(awake_icon(inhibitor_.active()), inhibitor_.active() ? "Keeping awake" : "Idle as usual");
        return true;
    }

    PowerUi::~PowerUi() {
        for (Action& a : actions_)
            if (a.reset_id)
                g_source_remove(a.reset_id);
    }

    void PowerUi::update() {
        const Power::Battery& b = power_.battery();
        const std::string text = battery_text(b);

        gtk_widget_set_visible(battery_footer_, b.present);
        gtk_widget_set_visible(battery_line_, b.present);
        if (b.present) {
            gtk_image_set_from_icon_name(GTK_IMAGE(battery_footer_icon_), b.icon.c_str());
            gtk_label_set_text(GTK_LABEL(battery_footer_label_), text.c_str());
            gtk_label_set_text(GTK_LABEL(battery_line_), text.c_str());
        }
        announce(last_, b);
        last_ = b;

        const bool has_profiles = !power_.profiles().empty();
        gtk_widget_set_visible(mode_tile_, has_profiles);
        gtk_widget_set_visible(profiles_, has_profiles);
        if (has_profiles) {
            gtk_image_set_from_icon_name(GTK_IMAGE(mode_icon_), profile_icon(power_.profile()));
            gtk_label_set_text(GTK_LABEL(mode_label_), profile_label(power_.profile()));
            rebuild_profiles();
        }
    }

    // Plugging in, unplugging, and running low are worth a glance; the first reading after start is not.
    void PowerUi::announce(const Power::Battery& before, const Power::Battery& now) {
        const BatteryChange c = battery_change(before, now, !seen_battery_);
        seen_battery_ |= now.present;
        const std::string pct = std::to_string(static_cast<int>(std::lround(now.percent))) + "%";
        if (c.recovered)
            island_.dismiss_alert();
        if (c.plugged)
            island_.show_event("battery-good-charging-symbolic", "Charging · " + pct);
        if (c.unplugged)
            island_.show_event(now.icon.c_str(), "On battery · " + pct);
        if (c.low)
            island_.show_alert("battery-caution-symbolic", "Battery low · " + battery_text(now));
    }

    void PowerUi::rebuild_profiles() {
        if (power_.profiles() != profiles_shown_) {
            profiles_shown_ = power_.profiles();
            while (GtkWidget* child = gtk_widget_get_first_child(profiles_))
                gtk_box_remove(GTK_BOX(profiles_), child);
            GtkToggleButton* group = nullptr;
            for (const std::string& p : profiles_shown_) {
                GtkWidget* b = gtk_toggle_button_new_with_label(profile_label(p));
                gtk_widget_add_css_class(b, "island-profile");
                if (group)
                    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), group);
                else
                    group = GTK_TOGGLE_BUTTON(b);
                g_object_set_data_full(G_OBJECT(b), "profile", g_strdup(p.c_str()), g_free);
                g_signal_connect(b,
                                 "toggled",
                                 G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
                                     auto* self = static_cast<PowerUi*>(data);
                                     if (!self->updating_ && gtk_toggle_button_get_active(b))
                                         self->power_.set_profile(
                                             static_cast<const char*>(g_object_get_data(G_OBJECT(b), "profile")));
                                 }),
                                 this);
                gtk_box_append(GTK_BOX(profiles_), b);
            }
        }
        updating_ = true;
        for (GtkWidget* b = gtk_widget_get_first_child(profiles_); b; b = gtk_widget_get_next_sibling(b))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b),
                                         power_.profile() ==
                                             static_cast<const char*>(g_object_get_data(G_OBJECT(b), "profile")));
        updating_ = false;
    }

    // Destructive actions ask once more in place, and forget the question after a few seconds.
    void PowerUi::on_action(GtkButton* button, gpointer data) {
        auto* self = static_cast<PowerUi*>(data);
        auto* a = static_cast<Action*>(g_object_get_data(G_OBJECT(button), "action"));
        if (a->confirm && !a->reset_id) {
            gtk_label_set_text(GTK_LABEL(a->text), a->confirm);
            gtk_widget_add_css_class(a->button, "confirm");
            a->reset_id = g_timeout_add(
                CONFIRM_MS,
                +[](gpointer data) -> gboolean {
                    auto* a = static_cast<Action*>(data);
                    a->reset_id = 0;
                    gtk_label_set_text(GTK_LABEL(a->text), a->label);
                    gtk_widget_remove_css_class(a->button, "confirm");
                    return G_SOURCE_REMOVE;
                },
                a);
            return;
        }
        if (a->reset_id) {
            g_source_remove(a->reset_id);
            a->reset_id = 0;
            gtk_label_set_text(GTK_LABEL(a->text), a->label);
            gtk_widget_remove_css_class(a->button, "confirm");
        }
        self->island_.close();
        a->run(*self);
    }

    // Fire and forget; polkit asks for a password itself when one is needed (interactive = true).
    void PowerUi::logind(const char* path, const char* iface, const char* method, GVariant* args) {
        GError* err = nullptr;
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
        if (!bus) {
            g_warning("power: no system bus: %s", err->message);
            g_error_free(err);
            if (args)
                g_variant_unref(g_variant_ref_sink(args));
            return;
        }
        g_dbus_connection_call(
            bus,
            "org.freedesktop.login1",
            path,
            iface,
            method,
            args,
            nullptr,
            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
            -1,
            nullptr,
            [](GObject* source, GAsyncResult* res, gpointer) {
                GError* err = nullptr;
                if (GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err))
                    g_variant_unref(r);
                if (err) {
                    g_warning("power: %s", err->message);
                    g_error_free(err);
                }
            },
            nullptr);
        g_object_unref(bus);
    }

} // namespace fenriz::bar
