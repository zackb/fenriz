#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include <memory>
#include <string>

#include "background.hpp"
#include "blur.hpp"
#include "brightness.hpp"
#include "config.hpp"
#include "history.hpp"
#include "idle.hpp"
#include "launcher.hpp"
#include "lock.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "notify.hpp"
#include "osd.hpp"
#include "polkit.hpp"
#include "power.hpp"
#include "screensaver.hpp"
#include "theme.hpp"
#include "volume.hpp"
#include "wallpaper_picker.hpp"

namespace {

    using fenriz::desktop::Background;
    using fenriz::desktop::Brightness;
    using fenriz::desktop::Config;
    using fenriz::desktop::History;
    using fenriz::desktop::Idle;
    using fenriz::desktop::Launcher;
    using fenriz::desktop::Lock;
    using fenriz::desktop::Notifications;
    using fenriz::desktop::Osd;
    using fenriz::desktop::OutputPower;
    using fenriz::desktop::Polkit;
    using fenriz::desktop::Screensaver;
    using fenriz::desktop::Volume;
    using fenriz::desktop::WallpaperPicker;

    struct Session {
        Config cfg;
        std::unique_ptr<Background> background;
        std::unique_ptr<WallpaperPicker> wallpaper;
        std::unique_ptr<Launcher> launcher;
        std::unique_ptr<Lock> lock;
        std::unique_ptr<Idle> idle;
        std::unique_ptr<Screensaver> screensaver;
        std::unique_ptr<Brightness> brightness;
        std::unique_ptr<OutputPower> power;
        std::unique_ptr<Polkit> polkit;
        std::unique_ptr<Osd> osd;
        std::unique_ptr<Volume> volume;
        std::unique_ptr<Notifications> notify;
        std::unique_ptr<History> history;
    };

    // Icons come from the theme's standard audio set, picked to match the level.
    const char* volume_icon(int percent, bool muted) {
        if (muted || percent == 0)
            return "audio-volume-muted-symbolic";
        if (percent < 34)
            return "audio-volume-low-symbolic";
        if (percent < 67)
            return "audio-volume-medium-symbolic";
        return "audio-volume-high-symbolic";
    }

    gboolean on_terminate(gpointer data) {
        g_application_quit(G_APPLICATION(data));
        return G_SOURCE_REMOVE;
    }

    void on_lock(GSimpleAction*, GVariant*, gpointer data) {
        auto* app = static_cast<GtkApplication*>(data);
        auto* session = static_cast<Session*>(g_object_get_data(G_OBJECT(app), "session"));
        if (session->lock)
            session->lock->engage();
    }

    void on_launcher(GSimpleAction*, GVariant*, gpointer data) {
        auto* app = static_cast<GtkApplication*>(data);
        auto* session = static_cast<Session*>(g_object_get_data(G_OBJECT(app), "session"));
        if (session->launcher)
            session->launcher->toggle(app);
    }

    gboolean prewarm_launcher(gpointer data) {
        auto* app = static_cast<GtkApplication*>(data);
        auto* session = static_cast<Session*>(g_object_get_data(G_OBJECT(app), "session"));
        if (session->launcher)
            session->launcher->prewarm(app);
        return G_SOURCE_REMOVE;
    }

    void on_notifications(GSimpleAction*, GVariant*, gpointer data) {
        auto* app = static_cast<GtkApplication*>(data);
        auto* session = static_cast<Session*>(g_object_get_data(G_OBJECT(app), "session"));
        if (session->history)
            session->history->toggle(app);
    }

    void on_wallpaper(GSimpleAction*, GVariant*, gpointer data) {
        auto* app = static_cast<GtkApplication*>(data);
        auto* session = static_cast<Session*>(g_object_get_data(G_OBJECT(app), "session"));
        if (session->wallpaper)
            session->wallpaper->toggle(app);
    }

    // Idempotent: the first invocation builds the desktop, later ones are routed
    // here by GApplication and must not rebuild it.
    void ensure_started(GtkApplication* app, Session* session) {
        if (session->background)
            return;

        fenriz::desktop::log::init();

        if (!gtk_layer_is_supported()) {
            g_printerr("fenriz-desktop: compositor does not support wlr-layer-shell\n");
            exit(1);
        }

        session->cfg = Config::load();
        fenriz::desktop::theme::install(session->cfg);
        if (session->cfg.shell_opacity < 1.0)
            fenriz::desktop::blur::init();

        if (!session->cfg.selected_wallpaper.empty() &&
            (!session->cfg.wallpaper.empty() || !session->cfg.output_wallpaper.empty()))
            g_message("wallpaper: using the picked %s; delete %s to fall back to the config",
                      session->cfg.selected_wallpaper.c_str(),
                      fenriz::desktop::wallpaper_state_path().c_str());
        fenriz::desktop::menu::install_actions(app);

        GSimpleAction* lock_action = g_simple_action_new("lock", nullptr);
        g_signal_connect(lock_action, "activate", G_CALLBACK(on_lock), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(lock_action));
        g_object_unref(lock_action);

        GSimpleAction* launcher_action = g_simple_action_new("launcher", nullptr);
        g_signal_connect(launcher_action, "activate", G_CALLBACK(on_launcher), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(launcher_action));
        g_object_unref(launcher_action);

        GSimpleAction* notifications_action = g_simple_action_new("notifications", nullptr);
        g_signal_connect(notifications_action, "activate", G_CALLBACK(on_notifications), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(notifications_action));
        g_object_unref(notifications_action);

        GSimpleAction* wallpaper_action = g_simple_action_new("wallpaper", nullptr);
        g_signal_connect(wallpaper_action, "activate", G_CALLBACK(on_wallpaper), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(wallpaper_action));
        g_object_unref(wallpaper_action);

        if (session->cfg.launcher) {
            session->launcher = std::make_unique<Launcher>(session->cfg);
            // Build the window off the critical path so the first keybind press only presents it.
            g_idle_add(prewarm_launcher, app);
        }

        session->lock = std::make_unique<Lock>(session->cfg);
        session->brightness = std::make_unique<Brightness>();
        session->osd = std::make_unique<Osd>();
        session->volume = std::make_unique<Volume>();
        if (!session->volume->start())
            g_message("volume: no PipeWire; the media keys fall back to whatever the config runs");
        if (session->cfg.idle_dim > 0 && !session->brightness->available())
            g_message("idle: no backlight to dim (external monitors need DDC/CI)");

        session->power = std::make_unique<OutputPower>();
        if (session->cfg.idle_dpms > 0)
            session->power->start();

        session->lock->set_wake_screens([session] {
            session->brightness->restore();
            session->power->set_all(true); // no-op unless idle_dpms armed the manager
        });

        if (session->cfg.idle_dpms > 0 && session->cfg.idle_lock > 0 && session->cfg.idle_dpms < session->cfg.idle_lock)
            g_warning("idle: idle_dpms (%ds) is before idle_lock (%ds), so the screens go dark "
                      "while the session is still unlocked",
                      session->cfg.idle_dpms,
                      session->cfg.idle_lock);

        session->idle = std::make_unique<Idle>();
        if (session->idle->start()) {
            const Config& cfg = session->cfg;
            // Each stage undoes itself on the first input after it fired
            session->idle->watch(
                cfg.idle_dim,
                [session] { session->brightness->dim_to(session->cfg.dim_brightness); },
                [session] { session->brightness->restore(); });
            session->idle->watch(cfg.idle_lock, [session] { session->lock->engage(); }, nullptr);
            session->idle->watch(
                cfg.idle_dpms,
                [session] { session->power->set_all(false); },
                [session] { session->power->set_all(true); });

            if (cfg.idle_dim > 0)
                g_message("idle: dimming after %d seconds", cfg.idle_dim);
            if (cfg.idle_lock > 0)
                g_message("idle: locking after %d seconds", cfg.idle_lock);
            if (cfg.idle_dpms > 0)
                g_message("idle: screens off after %d seconds", cfg.idle_dpms);

            // The Wayland idle-inhibit protocol is the compositor's job; this covers the DBus
            // half, which browsers and VLC prefer and would otherwise inhibit nothing.
            if (cfg.idle_dim > 0 || cfg.idle_lock > 0 || cfg.idle_dpms > 0) {
                session->screensaver = std::make_unique<Screensaver>([session](bool inhibited) {
                    g_message("idle: %s by a DBus inhibitor", inhibited ? "suspended" : "resumed");
                    session->idle->set_inhibited(inhibited);
                });
                session->screensaver->start();
            }
        }
        if (session->cfg.notifications) {
            session->notify = std::make_unique<Notifications>(app, session->cfg);
            session->notify->start();
            if (session->cfg.notify_history > 0)
                session->history = std::make_unique<History>(session->cfg, *session->notify);
        }
        session->polkit = std::make_unique<Polkit>(session->cfg);
        session->polkit->start();
        session->background = std::make_unique<Background>(session->cfg);
        session->background->start(app);
        if (!session->cfg.wallpaper_dir.empty())
            session->wallpaper = std::make_unique<WallpaperPicker>(session->cfg, *session->background);

        // nothing else holds the process alive once every surface is torn down.
        g_application_hold(G_APPLICATION(app));
    }

    int on_command_line(GtkApplication* app, GApplicationCommandLine* cmdline, gpointer data) {
        auto* session = static_cast<Session*>(data);
        ensure_started(app, session);

        int argc = 0;
        int status = 0; // client's exit code
        char** argv = g_application_command_line_get_arguments(cmdline, &argc);
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "lock") {
                session->lock->engage();
            } else if (arg == "launcher") {
                if (session->launcher)
                    session->launcher->toggle(app);
                else
                    g_warning("launcher is disabled in the config");
            } else if (arg == "notifications") {
                if (session->history)
                    session->history->toggle(app);
                else
                    g_warning("notification history is disabled in the config");
            } else if (arg == "wallpaper") {
                if (session->wallpaper)
                    session->wallpaper->toggle(app);
                else
                    g_warning("wallpaper_dir is not set in the config");
            } else if (arg == "brightness") {
                if (i + 1 >= argc) {
                    g_application_command_line_printerr(cmdline, "brightness needs a step, e.g. +5 or -5\n");
                    status = 1;
                    continue;
                }
                const int percent = session->brightness->adjust(atoi(argv[++i]));
                if (percent < 0) {
                    g_warning("brightness: no backlight to adjust (external monitors need DDC/CI)");
                    status = 1;
                } else {
                    session->osd->show(app, "display-brightness-symbolic", percent);
                }
            } else if (arg == "volume") {
                if (i + 1 >= argc) {
                    g_application_command_line_printerr(cmdline, "volume needs +N, -N, mute or micmute\n");
                    status = 1;
                    continue;
                }
                const std::string what = argv[++i];
                const bool mic = what == "micmute";
                int percent = -1;
                if (mic)
                    percent = session->volume->toggle_mic_mute();
                else if (what == "mute")
                    percent = session->volume->toggle_mute();
                else
                    percent = session->volume->adjust(atoi(what.c_str()));

                if (percent < 0) {
                    g_warning("volume: no audio to adjust");
                    status = 1;
                } else if (mic) {
                    session->osd->show(app,
                                       session->volume->mic_muted() ? "microphone-disabled-symbolic"
                                                                    : "audio-input-microphone-symbolic",
                                       percent);
                } else {
                    session->osd->show(app, volume_icon(percent, session->volume->muted()), percent);
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
    GtkApplication* app = gtk_application_new("dev.fenriz.Desktop", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_object_set_data(G_OBJECT(app), "session", &session);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), &session);

    g_unix_signal_add(SIGTERM, on_terminate, app);
    g_unix_signal_add(SIGINT, on_terminate, app);

    GError* err = nullptr;
    if (!g_application_register(G_APPLICATION(app), nullptr, &err)) {
        g_printerr("fenriz-desktop: %s\n", err->message);
        g_error_free(err);
        g_object_unref(app);
        return 1;
    }
    if (argc > 1 && !g_application_get_is_remote(G_APPLICATION(app))) {
        g_printerr("fenriz-desktop: not running. Start it from fenriz.conf with\n"
                   "  exec-once = fenriz-desktop\n");
        g_object_unref(app);
        return 1;
    }

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    session.polkit.reset();  // tear surfaces down while GTK is still alive
    session.history.reset(); // holds a callback into notify, so it goes first
    session.notify.reset();
    session.osd.reset();
    session.volume.reset();
    session.screensaver.reset();
    session.idle.reset();
    session.brightness.reset(); // undims if we are exiting while dimmed
    session.power.reset();
    session.lock.reset();
    session.launcher.reset();
    session.wallpaper.reset();
    session.background.reset();
    g_object_unref(app);
    return status;
}
