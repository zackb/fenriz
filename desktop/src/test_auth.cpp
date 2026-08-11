// The lock screen is the one place in this program where a bug is a vulnerability rather
// than an annoyance, so the rule it rests on gets asserted directly: nothing but PAM_SUCCESS
// opens a locked session.

#include <cassert>
#include <security/pam_appl.h>
#include <unistd.h>

#include "auth.hpp"

using fenriz::desktop::pam_result_unlocks;
using fenriz::desktop::pam_service_installed;
using fenriz::desktop::passive_services;

namespace {

    void test_only_success_unlocks() {
        assert(pam_result_unlocks(PAM_SUCCESS));

        // Every other documented return keeps the session locked. Listed one by one rather
        // than as `!= PAM_SUCCESS` so that adding a permissive case has to be deliberate.
        const int denied[] = {
            PAM_AUTH_ERR,     // wrong password
            PAM_USER_UNKNOWN, // no such user
            PAM_CRED_INSUFFICIENT,
            PAM_AUTHINFO_UNAVAIL, // backend down — a fingerprint reader that cannot answer
            PAM_MAXTRIES,
            PAM_ACCT_EXPIRED,
            PAM_NEW_AUTHTOK_REQD,
            PAM_PERM_DENIED,
            PAM_ABORT, // includes a missing /etc/pam.d service file
            PAM_BUF_ERR,
            PAM_CONV_ERR,
            PAM_SERVICE_ERR,
            PAM_SYSTEM_ERR,
            PAM_CRED_ERR,
            PAM_AUTHTOK_ERR,
            PAM_IGNORE, // "no opinion" is not consent
        };
        for (int rc : denied)
            assert(!pam_result_unlocks(rc));
    }

    // A garbage value must not be read as consent either.
    void test_unknown_codes_deny() {
        for (int rc = -50; rc < 200; rc++)
            if (rc != PAM_SUCCESS)
                assert(!pam_result_unlocks(rc));
    }

    // The lock refuses to engage when its service is missing, so a wrong answer here either
    // locks you out of a session nothing can unlock, or drops the guard entirely.
    void test_service_detection() {
        assert(!pam_service_installed("fenriz-desktop-definitely-not-installed"));
        assert(!pam_service_installed(""));
        assert(!pam_service_installed("../pam.d/other")); // a path is not a service name

        if (access("/etc/pam.d/other", R_OK) == 0)
            assert(pam_service_installed("other"));
    }

    // Fingerprint and face are raced as separate PAM services, picked purely by whether their
    // file exists. A bad name here either enables a method nobody installed or feeds
    // pam_service_installed() something that is not a service name.
    void test_passive_services() {
        assert(!passive_services().empty());
        for (const std::string& service : passive_services()) {
            assert(!service.empty());
            assert(service.find('/') == std::string::npos); // a path is not a service name

            // The password service must not be in the list: it would run a second time with an
            // empty secret, and a passive failure is deliberately not reported as a rejection.
            assert(service != "fenriz-desktop");
        }
    }

} // namespace

int main() {
    test_only_success_unlocks();
    test_unknown_codes_deny();
    test_service_detection();
    test_passive_services();
    return 0;
}
