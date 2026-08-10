#include <cassert>
#include <string>

#include "search.hpp"

using namespace fenriz::desktop;

namespace {

    // The two entries that motivated field search: neither is reachable by its common name.
    MatchFields gimp() {
        return MatchFields{"GNU Image Manipulation Program",
                           "Image Editor",
                           "Create images and edit photographs",
                           {"GIMP", "graphic", "design", "illustration", "painting"},
                           "gimp-3.2 %U"};
    }

    MatchFields pavucontrol() {
        return MatchFields{"Volume Control",
                           "Volume Control",
                           "Adjust the volume level",
                           {"pavucontrol", "PulseAudio", "Microphone", "Volume", "Mixer", "Headphones"},
                           "pavucontrol"};
    }

    void test_score_tiers() {
        assert(score_match("Volume Control", "volume control") == 1000); // exact, case-folded
        assert(score_match("Volume Control", "vol") == 800);             // whole string prefixed
        assert(score_match("Volume Control", "cont") == 600);            // a later word prefixed
        assert(score_match("Volume Control", "ume") == 200);             // substring only
        assert(score_match("Volume Control", "zzz") == -1);

        // Words also break on '-' and '_', not just whitespace.
        assert(score_match("gimp-3.2", "3.2") == 600);
        assert(score_match("qt5_settings", "settings") == 600);
    }

    // A one- or two-character substring matches almost every entry, so it must not rank.
    void test_short_substring_does_not_match() {
        assert(score_match("Volume Control", "ol") == -1);
        assert(score_match("Volume Control", "o") == -1);
        assert(score_match("Volume Control", "ume") == 200); // one more character and it does
        // Short queries still work as prefixes.
        assert(score_match("Volume Control", "v") == 800);
        assert(score_match("Volume Control", "co") == 600);
    }

    void test_empty_inputs_never_match() {
        assert(score_match("", "vol") == -1);
        assert(score_match("Volume Control", "") == -1);
        assert(match_score(pavucontrol(), "") == -1);
    }

    void test_gimp_is_reachable_by_its_real_name() {
        // The whole point: "gimp" appears nowhere in Name, only in Keywords and Exec.
        assert(match_score(gimp(), "gimp") == 980); // keyword exact, 1000 - 20
        assert(match_score(gimp(), "gim") == 780);  // keyword prefix, 800 - 20
        // The display name still matches as before.
        assert(match_score(gimp(), "gnu") == 800);
        assert(match_score(gimp(), "manip") == 600);
        // GenericName and Comment carry their penalties.
        assert(match_score(gimp(), "image") == 750);       // "Image Editor" prefix, 800 - 50
        assert(match_score(gimp(), "editor") == 550);      // "Image Editor" word, 600 - 50
        assert(match_score(gimp(), "photographs") == 500); // comment, 600 - 100
        assert(match_score(gimp(), "libreoffice") == -1);
    }

    void test_pavucontrol_is_reachable_by_every_alias() {
        assert(match_score(pavucontrol(), "pavu") == 780);        // keyword prefix
        assert(match_score(pavucontrol(), "pavucontrol") == 980); // keyword exact
        assert(match_score(pavucontrol(), "pulse") == 780);
        assert(match_score(pavucontrol(), "mixer") == 980);
        // An exact keyword hit (980) outranks a mere name prefix (800), as in quickshell.
        assert(match_score(pavucontrol(), "volume") == 980);
        assert(match_score(pavucontrol(), "control") == 600);
        assert(match_score(pavucontrol(), "gimp") == -1);
    }

    // An entry only findable through its command line ranks below every readable field.
    void test_exec_is_the_weakest_signal() {
        const MatchFields wrapped{"Terminal", "", "", {}, "/usr/bin/env foot --server"};
        assert(match_score(wrapped, "foot") == 180);
        assert(match_score(wrapped, "term") == 800); // the name still outranks it
        // Exec matches as a plain substring, with no length floor.
        assert(match_score(wrapped, "oot") == 180);
    }

    void test_better_field_wins() {
        // Same query, two entries: the one matching on Name must outscore the alias hit.
        const MatchFields by_name{"Mixer", "", "", {}, "mixer"};
        assert(match_score(by_name, "mixer") > match_score(pavucontrol(), "mixer"));
    }

    // Case folding is UTF-8 aware; accents are preserved
    void test_folding_ignores_case() {
        assert(score_match("LibreOffice Écrire", "libreoffice") == 800);
        assert(score_match("LibreOffice Écrire", "ÉCRIRE") == 600);
    }

} // namespace

int main() {
    test_score_tiers();
    test_short_substring_does_not_match();
    test_empty_inputs_never_match();
    test_gimp_is_reachable_by_its_real_name();
    test_pavucontrol_is_reachable_by_every_alias();
    test_exec_is_the_weakest_signal();
    test_better_field_wins();
    test_folding_ignores_case();
    return 0;
}
