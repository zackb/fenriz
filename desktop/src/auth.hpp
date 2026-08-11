#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fenriz::desktop {

    struct AuthAttempt; // defined in auth.cpp

    // The security invariant of this program: only PAM_SUCCESS unlocks a session.
    bool pam_result_unlocks(int pam_result);

    // Is a PAM service file installed for `service`?
    bool pam_service_installed(const std::string& service);

    // Services raced against the password: fingerprint, face. One module each, because PAM is serial.
    const std::vector<std::string>& passive_services();

    // Should `service` stay armed for as long as the session is locked?
    bool passive_service_persists(const std::string& service);

    // PAM authentication for the lock screen and the polkit agent.
    class Authenticator {
    public:
        // ok=false message=human-readable reason
        using Callback = std::function<void(bool ok, std::string message)>;

        // PAM message
        using Status = std::function<void(std::string message)>;

        explicit Authenticator(std::string service = "fenriz-desktop");
        ~Authenticator();

        Authenticator(const Authenticator&) = delete;
        Authenticator& operator=(const Authenticator&) = delete;

        // Ignored while another attempt is in flight
        void submit_password(std::string_view password, Callback done);

        // Starts every installed passive service (fingerprint, face) racing the password.
        void begin_passive(Callback done, Status status = {}, bool persistent_only = false);

        // Abandons any in-flight attempt's result, password and passive alike.
        void cancel();

        // Is a password attempt in flight? Passive attempts do not count: they must not lock out the entry.
        bool busy() const;

        const std::string& service() const { return service_; }

    private:
        std::string service_;
        std::shared_ptr<AuthAttempt> current_;
        std::vector<std::shared_ptr<AuthAttempt>> passive_;
    };

} // namespace fenriz::desktop
