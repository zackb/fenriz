#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace fenriz::desktop {

    struct AuthAttempt; // defined in auth.cpp

    // The security invariant of this program: only PAM_SUCCESS unlocks a session.
    bool pam_result_unlocks(int pam_result);

    // Is a PAM service file installed for `service`?
    bool pam_service_installed(const std::string& service);

    // PAM authentication for the lock screen and the polkit agent.
    class Authenticator {
    public:
        // ok=false message=human-readable reason
        using Callback = std::function<void(bool ok, std::string message)>;

        explicit Authenticator(std::string service = "fenriz-desktop");
        ~Authenticator();

        Authenticator(const Authenticator&) = delete;
        Authenticator& operator=(const Authenticator&) = delete;

        // Ignored while another attempt is in flight
        void submit_password(std::string_view password, Callback done);

        // TODO: Fingerprint/face backends start here and race the password.
        void begin_passive(Callback done);

        // Abandons any in-flight attempt's result.
        void cancel();

        bool busy() const;

        const std::string& service() const { return service_; }

    private:
        std::string service_;
        std::shared_ptr<AuthAttempt> current_;
    };

} // namespace fenriz::desktop
