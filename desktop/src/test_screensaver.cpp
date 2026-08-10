#include <cassert>

#include "screensaver.hpp"

using fenriz::desktop::screensaver_should_inhibit;

int main() {
    // What a video player sends while playing.
    assert(screensaver_should_inhibit("video-playing"));
    assert(screensaver_should_inhibit("Playing video"));

    // Music alone should not keep the screen awake.
    assert(!screensaver_should_inhibit("audio-playing"));
    assert(!screensaver_should_inhibit("AUDIO-PLAYING"));

    // Video wins when a client mentions both.
    assert(screensaver_should_inhibit("audio/video playing"));

    // Anything that is not identifiably audio-only inhibits: presentations, installers,
    // downloads, and clients that send no reason at all.
    assert(screensaver_should_inhibit(""));
    assert(screensaver_should_inhibit("presentation running"));

    return 0;
}
