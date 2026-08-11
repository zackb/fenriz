#include "auth.hpp"

#include <algorithm>
#include <atomic>
#include <glib.h>
#include <security/pam_appl.h>
#include <thread>
#include <unistd.h>

namespace fenriz::desktop {

    bool pam_result_unlocks(int pam_result) { return pam_result == PAM_SUCCESS; }

    bool pam_service_installed(const std::string& service) {
        if (service.empty() || service.find('/') != std::string::npos)
            return false;
        for (const char* dir : {"/etc/pam.d/", "/usr/lib/pam.d/"})
            if (access((dir + service).c_str(), R_OK) == 0)
                return true;
        return false;
    }

    const std::vector<std::string>& passive_services() {
        static const std::vector<std::string> services = {
            "fenriz-desktop-fprint", // pam_fprintd.so
            "fenriz-desktop-gaze",   // pam_gaze.so
        };
        return services;
    }

    struct AuthAttempt {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> running{true};
        Authenticator::Callback done;
        Authenticator::Status status;
        std::string service;
        std::string user;
        std::string password;
        bool passive = false; // fingerprint or face: no secret to type, failure is not a rejection
        int result = PAM_AUTH_ERR;
        std::string message;
    };

    namespace {

        using AttemptPtr = std::shared_ptr<AuthAttempt>;

        struct StatusPost {
            AttemptPtr attempt;
            std::string text;
        };

        // Runs on the GTK main loop.
        gboolean deliver_status(gpointer data) {
            std::unique_ptr<StatusPost> post(static_cast<StatusPost*>(data));
            if (!post->attempt->cancelled && post->attempt->status)
                post->attempt->status(post->text);
            return G_SOURCE_REMOVE;
        }

        void post_status(const AttemptPtr& attempt, const char* text) {
            if (attempt->status && text && *text)
                g_idle_add(deliver_status, new StatusPost{attempt, text});
        }

        void free_replies(pam_response* replies, int n) {
            for (int i = 0; i < n; i++)
                free(replies[i].resp);
            free(replies);
        }

        // PAM asks for the secret through this, and fprintd talks back through it:
        // "Place your finger on the reader" is a PAM_TEXT_INFO and nothing else.
        int converse(int n, const pam_message** msgs, pam_response** out, void* appdata) {
            const AttemptPtr& attempt = *static_cast<AttemptPtr*>(appdata);
            if (n <= 0 || !msgs || !out)
                return PAM_CONV_ERR;

            auto* replies = static_cast<pam_response*>(calloc(n, sizeof(pam_response)));
            if (!replies)
                return PAM_BUF_ERR;

            for (int i = 0; i < n; i++) {
                switch (msgs[i]->msg_style) {
                case PAM_PROMPT_ECHO_OFF:
                case PAM_PROMPT_ECHO_ON:
                    if (attempt->passive) {
                        post_status(attempt, msgs[i]->msg);
                        free_replies(replies, n);
                        return PAM_CONV_ERR;
                    }
                    replies[i].resp = strdup(attempt->password.c_str());
                    if (!replies[i].resp) {
                        free_replies(replies, n);
                        return PAM_BUF_ERR;
                    }
                    break;
                case PAM_ERROR_MSG:
                case PAM_TEXT_INFO:
                    post_status(attempt, msgs[i]->msg);
                    break; // nothing to answer
                default:
                    free_replies(replies, n);
                    return PAM_CONV_ERR;
                }
            }
            *out = replies;
            return PAM_SUCCESS;
        }

        void run_pam(const AttemptPtr& attempt) {
            AttemptPtr self = attempt;
            pam_conv conv = {converse, &self};
            pam_handle_t* pamh = nullptr;

            int rc = pam_start(attempt->service.c_str(), attempt->user.c_str(), &conv, &pamh);
            if (rc != PAM_SUCCESS) {
                attempt->result = rc;
                attempt->message = "cannot start PAM";
                return;
            }
            rc = pam_authenticate(pamh, 0);
            if (rc == PAM_SUCCESS)
                rc = pam_acct_mgmt(pamh, 0); // expired or disabled accounts must not unlock

            attempt->result = rc;
            attempt->message = rc == PAM_SUCCESS ? "" : pam_strerror(pamh, rc);
            pam_end(pamh, rc);
        }

        // Runs on the GTK main loop. Owns the shared_ptr handed over by the worker thread.
        gboolean deliver(gpointer data) {
            std::unique_ptr<AttemptPtr> held(static_cast<AttemptPtr*>(data));
            AuthAttempt& attempt = **held;
            attempt.running = false;
            if (attempt.cancelled)
                return G_SOURCE_REMOVE;

            const bool ok = pam_result_unlocks(attempt.result);
            // a passive method saying no is not a rejected login
            if (!ok && attempt.passive) {
                if (attempt.status && !attempt.message.empty())
                    attempt.status(attempt.message);
                return G_SOURCE_REMOVE;
            }
            if (attempt.done)
                attempt.done(ok, attempt.message);
            return G_SOURCE_REMOVE;
        }

        void start(const AttemptPtr& attempt) {
            // detached on purpose: pam_authenticate cannot be interrupted
            std::thread([attempt] {
                run_pam(attempt);
                g_idle_add(deliver, new AttemptPtr(attempt));
            }).detach();
        }

        std::string current_user() { return g_get_user_name() ? g_get_user_name() : ""; }

    } // namespace

    Authenticator::Authenticator(std::string service) : service_(std::move(service)) {}

    Authenticator::~Authenticator() { cancel(); }

    bool Authenticator::busy() const { return current_ && current_->running; }

    void Authenticator::cancel() {
        if (current_)
            current_->cancelled = true;
        current_.reset();
        for (const AttemptPtr& attempt : passive_)
            attempt->cancelled = true;
        // cancelled but still running attempts stay tracked: their device is still claimed.
        std::erase_if(passive_, [](const AttemptPtr& a) { return !a->running; });
    }

    void Authenticator::submit_password(std::string_view password, Callback done) {
        if (busy())
            return;

        auto attempt = std::make_shared<AuthAttempt>();
        attempt->service = service_;
        attempt->user = current_user();
        attempt->password = std::string(password);
        attempt->done = std::move(done);
        current_ = attempt;
        start(attempt);
    }

    void Authenticator::begin_passive(Callback done, Status status) {
        std::erase_if(passive_, [](const AttemptPtr& a) { return !a->running; });

        for (const std::string& service : passive_services()) {
            // no service file means the method is not set up on this machine
            if (!pam_service_installed(service))
                continue;
            if (std::any_of(
                    passive_.begin(), passive_.end(), [&](const AttemptPtr& a) { return a->service == service; }))
                continue;

            auto attempt = std::make_shared<AuthAttempt>();
            attempt->service = service;
            attempt->user = current_user();
            attempt->passive = true;
            attempt->done = done;
            attempt->status = status;
            passive_.push_back(attempt);
            start(attempt);
        }
    }

} // namespace fenriz::desktop
