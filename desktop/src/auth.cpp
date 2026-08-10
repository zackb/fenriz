#include "auth.hpp"

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

    struct AuthAttempt {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> running{true};
        Authenticator::Callback done;
        std::string service;
        std::string user;
        std::string password;
        int result = PAM_AUTH_ERR;
        std::string message;
    };

    namespace {

        // PAM asks for the secret through this
        int converse(int n, const pam_message** msgs, pam_response** out, void* appdata) {
            auto* attempt = static_cast<AuthAttempt*>(appdata);
            if (n <= 0 || !msgs || !out)
                return PAM_CONV_ERR;

            auto* replies = static_cast<pam_response*>(calloc(n, sizeof(pam_response)));
            if (!replies)
                return PAM_BUF_ERR;

            for (int i = 0; i < n; i++) {
                switch (msgs[i]->msg_style) {
                case PAM_PROMPT_ECHO_OFF:
                case PAM_PROMPT_ECHO_ON:
                    replies[i].resp = strdup(attempt->password.c_str());
                    if (!replies[i].resp) {
                        free(replies);
                        return PAM_BUF_ERR;
                    }
                    break;
                case PAM_ERROR_MSG:
                case PAM_TEXT_INFO:
                    break; // nothing to answer
                default:
                    free(replies);
                    return PAM_CONV_ERR;
                }
            }
            *out = replies;
            return PAM_SUCCESS;
        }

        void run_pam(const std::shared_ptr<AuthAttempt>& attempt) {
            pam_conv conv = {converse, attempt.get()};
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
            std::unique_ptr<std::shared_ptr<AuthAttempt>> held(static_cast<std::shared_ptr<AuthAttempt>*>(data));
            AuthAttempt& attempt = **held;
            attempt.running = false;
            if (!attempt.cancelled && attempt.done)
                attempt.done(pam_result_unlocks(attempt.result), attempt.message);
            return G_SOURCE_REMOVE;
        }

    } // namespace

    Authenticator::Authenticator(std::string service) : service_(std::move(service)) {}

    Authenticator::~Authenticator() { cancel(); }

    bool Authenticator::busy() const { return current_ && current_->running; }

    void Authenticator::cancel() {
        if (current_)
            current_->cancelled = true;
        current_.reset();
    }

    void Authenticator::submit_password(std::string_view password, Callback done) {
        if (busy())
            return;

        auto attempt = std::make_shared<AuthAttempt>();
        attempt->service = service_;
        attempt->user = g_get_user_name() ? g_get_user_name() : "";
        attempt->password = std::string(password);
        attempt->done = std::move(done);
        current_ = attempt;

        // Detached on purpose: pam_authenticate cannot be interrupted
        std::thread([attempt] {
            run_pam(attempt);
            g_idle_add(deliver, new std::shared_ptr<AuthAttempt>(attempt));
        }).detach();
    }

    void Authenticator::begin_passive(Callback) {
        // TODO: fprintd and gaze attach here
    }

} // namespace fenriz::desktop
