#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include <memory>
#include <string>
#include <vector>

#include "audio.hpp"
#include "audio_ui.hpp"
#include "bar.hpp"
#include "bluetooth.hpp"
#include "bluetooth_ui.hpp"
#include "blur.hpp"
#include "brightness.hpp"
#include "compositor.hpp"
#include "config.hpp"
#include "inhibit.hpp"
#include "island.hpp"
#include "log.hpp"
#include "media_ui.hpp"
#include "mpris.hpp"
#include "network.hpp"
#include "plugin_ui.hpp"
#include "power_ui.hpp"
#include "sysstat.hpp"
#include "system_ui.hpp"
#include "theme.hpp"
#include "tray.hpp"
#include "tray_ui.hpp"
#include "upower.hpp"
#include "wifi_ui.hpp"

namespace {

    using fenriz::bar::Audio;
    using fenriz::bar::AudioUi;
    using fenriz::bar::Bar;
    using fenriz::bar::Bluetooth;
    using fenriz::bar::BluetoothUi;
    using fenriz::bar::Compositor;
    using fenriz::bar::IdleInhibitor;
    using fenriz::bar::Island;
    using fenriz::bar::MediaUi;
    using fenriz::bar::Mpris;
    using fenriz::bar::Network;
    using fenriz::bar::PluginUi;
    using fenriz::bar::Power;
    using fenriz::bar::PowerUi;
    using fenriz::bar::SysStat;
    using fenriz::bar::SystemUi;
    using fenriz::bar::Tray;
    using fenriz::bar::TrayUi;
    using fenriz::bar::WifiUi;
    using fenriz::desktop::Brightness;
    using fenriz::desktop::Config;

    struct Session {
        Config cfg;
        std::unique_ptr<Compositor> compositor;
        std::unique_ptr<Audio> audio;
        std::unique_ptr<Brightness> brightness;
        std::unique_ptr<Mpris> mpris;
        std::unique_ptr<Power> power;
        std::unique_ptr<Bluetooth> bluetooth;
        std::unique_ptr<Network> network;
        std::unique_ptr<Tray> tray;
        std::unique_ptr<TrayUi> tray_ui;
        std::unique_ptr<SysStat> stats;
        std::unique_ptr<Island> island;
        std::unique_ptr<IdleInhibitor> inhibitor;
        std::unique_ptr<WifiUi> wifi_ui;
        std::unique_ptr<BluetoothUi> bluetooth_ui;
        std::unique_ptr<SystemUi> system_ui;
        std::unique_ptr<PowerUi> power_ui;
        std::unique_ptr<AudioUi> audio_ui;
        std::unique_ptr<MediaUi> media_ui;
        std::vector<std::unique_ptr<PluginUi>> plugins;
        std::unique_ptr<Bar> bar;
    };

    // fenriz-desktop hands its media-key levels here: org.gtk.Actions.Activate("osd", [<("icon", percent)>]).
    void on_osd(GSimpleAction*, GVariant* param, gpointer data) {
        auto* session = static_cast<Session*>(data);
        const char* icon = nullptr;
        int percent = 0;
        g_variant_get(param, "(&si)", &icon, &percent);
        session->island->show_osd(icon, percent);
        if (g_str_has_prefix(icon, "fenriz-brightness"))
            session->audio_ui->brightness_changed(percent);
    }

    // Plugin poll timers freeze across suspend, so they are told when the machine wakes.
    void on_prepare_for_sleep(
        GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* params, gpointer data) {
        gboolean sleeping = FALSE;
        g_variant_get(params, "(b)", &sleeping);
        if (sleeping)
            return;
        for (auto& plugin : static_cast<Session*>(data)->plugins)
            plugin->send(fenriz::bar::plugin_event("resume"));
    }

    void start_plugins(Session* session) {
        for (const auto& [name, command] : session->cfg.plugins) {
            if (name == "home") {
                g_warning("plugin %s: home is not a plugin page", name.c_str());
                continue;
            }
            session->plugins.push_back(std::make_unique<PluginUi>(*session->island, name, command));
        }
        if (session->plugins.empty())
            return;
        if (GDBusConnection* system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr)) {
            g_dbus_connection_signal_subscribe(system,
                                               "org.freedesktop.login1",
                                               "org.freedesktop.login1.Manager",
                                               "PrepareForSleep",
                                               "/org/freedesktop/login1",
                                               nullptr,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               on_prepare_for_sleep,
                                               session,
                                               nullptr);
            g_object_unref(system); // the bus singleton outlives this reference and keeps the subscription
        }
    }

    gboolean on_terminate(gpointer data) {
        g_application_quit(G_APPLICATION(data));
        return G_SOURCE_REMOVE;
    }

    // Idempotent: later invocations are routed here by GApplication and must not rebuild the bar.
    void ensure_started(GtkApplication* app, Session* session) {
        if (session->bar)
            return;

        fenriz::desktop::log::init("fenriz-bar");
        if (!gtk_layer_is_supported()) {
            g_printerr("fenriz-bar: compositor does not support wlr-layer-shell\n");
            exit(1);
        }

        session->cfg = Config::load();
        fenriz::desktop::theme::install(session->cfg);
        if (session->cfg.shell_opacity < 1.0)
            fenriz::desktop::blur::init();

        session->compositor = std::make_unique<Compositor>();
        session->audio = std::make_unique<Audio>();
        session->brightness = std::make_unique<Brightness>();
        session->mpris = std::make_unique<Mpris>();
        session->power = std::make_unique<Power>();
        session->bluetooth = std::make_unique<Bluetooth>();
        session->network = std::make_unique<Network>();
        session->tray = std::make_unique<Tray>();
        session->tray_ui = std::make_unique<TrayUi>(*session->tray);
        session->stats = std::make_unique<SysStat>();
        session->island = std::make_unique<Island>();
        session->island->start(app);
        session->inhibitor = std::make_unique<IdleInhibitor>(session->island->window());
        session->wifi_ui = std::make_unique<WifiUi>(*session->island, *session->network);
        session->bluetooth_ui = std::make_unique<BluetoothUi>(*session->island, *session->bluetooth);
        // the footer reads left to right in construction order: system readout, then battery and power
        session->system_ui = std::make_unique<SystemUi>(*session->island, *session->stats);
        session->power_ui =
            std::make_unique<PowerUi>(*session->island, *session->power, *session->compositor, *session->inhibitor);
        session->audio_ui = std::make_unique<AudioUi>(*session->island, *session->audio, *session->brightness);
        session->media_ui = std::make_unique<MediaUi>(*session->island, *session->mpris);
        start_plugins(session);
        session->bar = std::make_unique<Bar>(*session->compositor,
                                             *session->island,
                                             *session->audio,
                                             *session->mpris,
                                             *session->power,
                                             *session->bluetooth,
                                             *session->network,
                                             *session->tray_ui);
        session->bar->start(app);
        session->compositor->start([session](const auto& state) { session->bar->update(state); });
        session->audio->start();
        session->mpris->start(g_application_get_dbus_connection(G_APPLICATION(app)));
        session->power->start();
        session->bluetooth->start();
        session->network->start();
        session->tray->start(g_application_get_dbus_connection(G_APPLICATION(app)));

        GSimpleAction* osd = g_simple_action_new("osd", G_VARIANT_TYPE("(si)"));
        g_signal_connect(osd, "activate", G_CALLBACK(on_osd), session);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(osd));
        g_object_unref(osd);

        g_application_hold(G_APPLICATION(app));
    }

    int on_command_line(GtkApplication* app, GApplicationCommandLine* cmdline, gpointer data) {
        auto* session = static_cast<Session*>(data);
        ensure_started(app, session);

        int argc = 0;
        int status = 0; // the client's exit code
        char** argv = g_application_command_line_get_arguments(cmdline, &argc);
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "open") {
                const std::string page = i + 1 < argc ? argv[++i] : "home";
                if (!session->island->has_page(page)) {
                    g_application_command_line_printerr(cmdline, "no such page: %s\n", page.c_str());
                    status = 1;
                    continue;
                }
                // on the screen with the focused window, like a keybind expects
                const std::string output = session->compositor->state().focused_output();
                session->island->toggle(page, session->bar->monitor_for(output));
            } else if (arg == "close") {
                session->island->close();
            } else if (arg == "awake") {
                if (!session->power_ui->toggle_awake()) {
                    g_application_command_line_printerr(cmdline, "the compositor does not support idle inhibit\n");
                    status = 1;
                }
            } else {
                g_application_command_line_printerr(cmdline, "unknown command: %s\n", argv[i]);
                status = 1;
            }
        }
        g_strfreev(argv);
        return status;
    }

} // namespace

int main(int argc, char** argv) {
    Session session;
    GtkApplication* app = gtk_application_new("dev.fenriz.Bar", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), &session);

    g_unix_signal_add(SIGTERM, on_terminate, app);
    g_unix_signal_add(SIGINT, on_terminate, app);

    GError* err = nullptr;
    if (!g_application_register(G_APPLICATION(app), nullptr, &err)) {
        g_printerr("fenriz-bar: %s\n", err->message);
        g_error_free(err);
        g_object_unref(app);
        return 1;
    }
    if (argc > 1 && !g_application_get_is_remote(G_APPLICATION(app))) {
        g_printerr("fenriz-bar: not running. Start it from fenriz.conf with\n"
                   "  exec-once = fenriz-bar\n");
        g_object_unref(app);
        return 1;
    }

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    session.bar.reset(); // tear surfaces down while GTK is still alive
    session.tray_ui.reset();
    session.tray.reset();
    session.plugins.clear();
    session.media_ui.reset();
    session.audio_ui.reset();
    session.power_ui.reset();
    session.bluetooth_ui.reset();
    session.wifi_ui.reset();
    session.system_ui.reset();
    session.inhibitor.reset();
    session.island.reset();
    session.stats.reset();
    session.power.reset();
    session.bluetooth.reset();
    session.network.reset();
    session.mpris.reset();
    session.audio.reset();
    session.brightness.reset();
    session.compositor.reset();
    g_object_unref(app);
    return status;
}
