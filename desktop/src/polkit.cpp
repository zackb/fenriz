#include "polkit.hpp"

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE

#include <gtk4-layer-shell.h>
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>
#include <unistd.h>

#include "blur.hpp"
#include "theme.hpp"

namespace {

    namespace blur = fenriz::desktop::blur;

    // One in-flight authentication
    struct Request {
        GTask* task = nullptr;
        PolkitAgentSession* session = nullptr;
        GtkWindow* window = nullptr;
        GtkWidget* entry = nullptr;
        GtkWidget* prompt = nullptr;
        GtkWidget* error = nullptr;
        bool completed = false;
    };

    void request_finish(Request* req, bool authorized) {
        if (req->completed)
            return;
        req->completed = true;
        if (req->window) {
            gtk_window_destroy(req->window);
            req->window = nullptr;
        }
        if (req->task) {
            g_task_return_boolean(req->task, authorized);
            g_clear_object(&req->task);
        }
        if (req->session) {
            g_signal_handlers_disconnect_by_data(req->session, req);
            g_clear_object(&req->session);
        }
        delete req;
    }

    // The session wants input.
    void on_request(PolkitAgentSession*, const gchar* text, gboolean echo_on, gpointer data) {
        auto* req = static_cast<Request*>(data);
        if (!req->window)
            return;
        gtk_label_set_text(GTK_LABEL(req->prompt), text ? text : "Password:");
        gtk_editable_set_text(GTK_EDITABLE(req->entry), "");
        gtk_editable_set_editable(GTK_EDITABLE(req->entry), TRUE);
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(req->entry), echo_on);
        gtk_window_set_focus(req->window, req->entry);
    }

    void on_show_error(PolkitAgentSession*, const gchar* text, gpointer data) {
        auto* req = static_cast<Request*>(data);
        if (req->error && text)
            gtk_label_set_text(GTK_LABEL(req->error), text);
    }

    void on_show_info(PolkitAgentSession*, const gchar* text, gpointer data) {
        auto* req = static_cast<Request*>(data);
        if (req->error && text)
            gtk_label_set_text(GTK_LABEL(req->error), text);
    }

    // The only place authorisation is decided, and polkitd decided it.
    void on_completed(PolkitAgentSession*, gboolean gained_authorization, gpointer data) {
        request_finish(static_cast<Request*>(data), gained_authorization);
    }

    void submit(Request* req) {
        if (!req->session || !req->entry)
            return;
        const char* text = gtk_editable_get_text(GTK_EDITABLE(req->entry));
        gtk_editable_set_editable(GTK_EDITABLE(req->entry), FALSE);
        gtk_label_set_text(GTK_LABEL(req->error), "");
        polkit_agent_session_response(req->session, text ? text : "");
        gtk_editable_set_text(GTK_EDITABLE(req->entry), "");
    }

    void on_entry_activate(GtkWidget*, gpointer data) { submit(static_cast<Request*>(data)); }

    void on_authenticate_clicked(GtkButton*, gpointer data) { submit(static_cast<Request*>(data)); }

    void on_cancel_clicked(GtkButton*, gpointer data) {
        auto* req = static_cast<Request*>(data);
        if (req->session)
            polkit_agent_session_cancel(req->session); // ::completed follows with FALSE
        else
            request_finish(req, false);
    }

    gboolean on_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) {
        if (keyval != GDK_KEY_Escape)
            return FALSE;
        on_cancel_clicked(nullptr, data);
        return TRUE;
    }

    // Layer-shell overlay rather than a toplevel
    void build_dialog(Request* req, const char* message) {
        req->window = GTK_WINDOW(gtk_window_new());
        gtk_layer_init_for_window(req->window);
        gtk_layer_set_namespace(req->window, "fenriz-polkit");
        gtk_layer_set_layer(req->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(req->window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_widget_add_css_class(GTK_WIDGET(req->window), "fenriz-shell");

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_add_css_class(box, "fenriz-polkit");
        gtk_widget_set_size_request(box, 420, -1);

        GtkWidget* title = gtk_label_new(message ? message : "Authentication required");
        gtk_label_set_wrap(GTK_LABEL(title), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(title), 44);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_widget_add_css_class(title, "fenriz-polkit-title");
        gtk_box_append(GTK_BOX(box), title);

        req->prompt = gtk_label_new("Password:");
        gtk_label_set_xalign(GTK_LABEL(req->prompt), 0.0);
        gtk_box_append(GTK_BOX(box), req->prompt);

        req->entry = gtk_password_entry_new();
        gtk_widget_add_css_class(req->entry, "fenriz-field");
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(req->entry), FALSE);
        g_signal_connect(req->entry, "activate", G_CALLBACK(on_entry_activate), req);
        gtk_box_append(GTK_BOX(box), req->entry);

        req->error = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(req->error), TRUE);
        gtk_label_set_xalign(GTK_LABEL(req->error), 0.0);
        gtk_widget_add_css_class(req->error, "fenriz-polkit-error");
        gtk_box_append(GTK_BOX(box), req->error);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);
        GtkWidget* cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), req);
        gtk_box_append(GTK_BOX(buttons), cancel);
        GtkWidget* ok = gtk_button_new_with_label("Authenticate");
        gtk_widget_add_css_class(ok, "suggested-action");
        g_signal_connect(ok, "clicked", G_CALLBACK(on_authenticate_clicked), req);
        gtk_box_append(GTK_BOX(buttons), ok);
        gtk_box_append(GTK_BOX(box), buttons);

        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), req);
        gtk_widget_add_controller(GTK_WIDGET(req->window), GTK_EVENT_CONTROLLER(keys));

        gtk_window_set_child(req->window, box);
        gtk_window_set_focus(req->window, req->entry);
        blur::attach(GTK_NATIVE(req->window), box, blur::Mode::Widget, fenriz::desktop::theme::CARD_RADIUS);
        gtk_window_present(req->window);
    }

    // Prefer authenticating as the current user when polkit offers a choice
    PolkitIdentity* pick_identity(GList* identities) {
        PolkitIdentity* first = nullptr;
        for (GList* l = identities; l; l = l->next) {
            auto* id = static_cast<PolkitIdentity*>(l->data);
            if (!first)
                first = id;
            if (POLKIT_IS_UNIX_USER(id) &&
                polkit_unix_user_get_uid(POLKIT_UNIX_USER(id)) == static_cast<uid_t>(getuid()))
                return id;
        }
        return first;
    }

} // namespace

struct _FenrizPolkitListener {
    PolkitAgentListener parent;
};

G_DECLARE_FINAL_TYPE(FenrizPolkitListener, fenriz_polkit_listener, FENRIZ, POLKIT_LISTENER, PolkitAgentListener)
G_DEFINE_TYPE(FenrizPolkitListener, fenriz_polkit_listener, POLKIT_AGENT_TYPE_LISTENER)

static void fenriz_polkit_listener_init(FenrizPolkitListener*) {}

static void initiate_authentication(PolkitAgentListener* listener,
                                    const gchar* action_id,
                                    const gchar* message,
                                    const gchar* icon_name,
                                    PolkitDetails* details,
                                    const gchar* cookie,
                                    GList* identities,
                                    GCancellable* cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data) {
    (void)action_id;
    (void)icon_name;
    (void)details;

    auto* req = new Request{};
    req->task = g_task_new(listener, cancellable, callback, user_data);

    PolkitIdentity* identity = pick_identity(identities);
    if (!identity) {
        g_warning("polkit: no identity offered for this action");
        request_finish(req, false);
        return;
    }

    build_dialog(req, message);

    req->session = polkit_agent_session_new(identity, cookie);
    g_signal_connect(req->session, "request", G_CALLBACK(on_request), req);
    g_signal_connect(req->session, "show-error", G_CALLBACK(on_show_error), req);
    g_signal_connect(req->session, "show-info", G_CALLBACK(on_show_info), req);
    g_signal_connect(req->session, "completed", G_CALLBACK(on_completed), req);
    polkit_agent_session_initiate(req->session);
}

static gboolean initiate_authentication_finish(PolkitAgentListener*, GAsyncResult* res, GError** error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void fenriz_polkit_listener_class_init(FenrizPolkitListenerClass* klass) {
    PolkitAgentListenerClass* listener_class = POLKIT_AGENT_LISTENER_CLASS(klass);
    listener_class->initiate_authentication = initiate_authentication;
    listener_class->initiate_authentication_finish = initiate_authentication_finish;
}

namespace fenriz::desktop {

    Polkit::Polkit(const Config& cfg) : cfg_(cfg) {}

    Polkit::~Polkit() {
        if (registration_)
            polkit_agent_listener_unregister(registration_);
        if (listener_)
            g_object_unref(listener_);
    }

    void Polkit::start() {
        GError* err = nullptr;
        PolkitSubject* session = polkit_unix_session_new_for_process_sync(getpid(), nullptr, &err);
        if (!session) {
            g_warning("polkit: cannot find this logind session: %s", err ? err->message : "unknown");
            g_clear_error(&err);
            return;
        }

        listener_ = g_object_new(fenriz_polkit_listener_get_type(), nullptr);
        registration_ = polkit_agent_listener_register(
            POLKIT_AGENT_LISTENER(listener_), POLKIT_AGENT_REGISTER_FLAGS_NONE, session, nullptr, nullptr, &err);
        g_object_unref(session);

        if (!registration_) {
            g_warning("polkit: not registering an agent: %s", err ? err->message : "unknown");
            g_clear_error(&err);
            g_clear_object(&listener_);
            return;
        }
        g_message("polkit: authentication agent registered");
    }

} // namespace fenriz::desktop
