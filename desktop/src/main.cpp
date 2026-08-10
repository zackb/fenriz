#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include <memory>
#include <string>

#include "background.hpp"
#include "config.hpp"
#include "idle.hpp"
#include "launcher.hpp"
#include "lock.hpp"
#include "menu.hpp"
#include "polkit.hpp"

namespace {

    using fenriz::desktop::Background;
    using fenriz::desktop::Config;
    using fenriz::desktop::Idle;
    using fenriz::desktop::Launcher;
    using fenriz::desktop::Lock;
    using fenriz::desktop::Polkit;

    struct Session {
        Config cfg;
        std::unique_ptr<Background> background;
        std::unique_ptr<Launcher> launcher;
        std::unique_ptr<Lock> lock;
        std::unique_ptr<Idle> idle;
        std::unique_ptr<Polkit> polkit;
    };

    const char* CSS = ".fenriz-background { background: transparent; }";

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

    // Idempotent: the first invocation builds the desktop, later ones are routed
    // here by GApplication and must not rebuild it.
    void ensure_started(GtkApplication* app, Session* session) {
        if (session->background)
            return;

        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css, CSS);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);

        session->cfg = Config::load();
        fenriz::desktop::menu::install_actions(app);

        GSimpleAction* lock_action = g_simple_action_new("lock", nullptr);
        g_signal_connect(lock_action, "activate", G_CALLBACK(on_lock), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(lock_action));
        g_object_unref(lock_action);

        GSimpleAction* launcher_action = g_simple_action_new("launcher", nullptr);
        g_signal_connect(launcher_action, "activate", G_CALLBACK(on_launcher), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(launcher_action));
        g_object_unref(launcher_action);

        if (session->cfg.launcher)
            session->launcher = std::make_unique<Launcher>(session->cfg);

        session->lock = std::make_unique<Lock>(session->cfg);
        session->idle = std::make_unique<Idle>(session->cfg);
        session->idle->start([session] { session->lock->engage(); });
        session->polkit = std::make_unique<Polkit>(session->cfg);
        session->polkit->start();
        session->background = std::make_unique<Background>(session->cfg);
        session->background->start(app);

        // nothing else holds the process alive once every surface is torn down.
        g_application_hold(G_APPLICATION(app));
    }

    int on_command_line(GtkApplication* app, GApplicationCommandLine* cmdline, gpointer data) {
        auto* session = static_cast<Session*>(data);
        ensure_started(app, session);

        int argc = 0;
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
            } else {
                g_application_command_line_printerr(cmdline, "unknown command: %s\n", argv[i]);
            }
        }
        g_strfreev(argv);
        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    if (!gtk_init_check()) {
        g_printerr("fenriz-desktop: no display\n");
        return 1;
    }
    if (!gtk_layer_is_supported()) {
        g_printerr("fenriz-desktop: compositor does not support wlr-layer-shell\n");
        return 1;
    }

    Session session;
    GtkApplication* app = gtk_application_new("dev.fenriz.Desktop", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_object_set_data(G_OBJECT(app), "session", &session);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), &session);

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
    session.polkit.reset(); // tear surfaces down while GTK is still alive
    session.idle.reset();
    session.lock.reset();
    session.launcher.reset();
    session.background.reset();
    g_object_unref(app);
    return status;
}
