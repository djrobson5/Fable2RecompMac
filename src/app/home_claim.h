// Fable 2: the Home button and the window, for a launcher (Wayfarer) that starts
// the app. macOS takes the Home button on a game controller as a system gesture
// (it opens the Games app) unless the frontmost process turns that off.

#pragma once

namespace fable2::app {

// Turns off the system gesture on the Home button of every game controller,
// now and for any connected later. FABLE2_CLAIM_HOME=0 leaves it alone. Call
// once, on the main (UI) thread. `log` (may be null) gets one line per claim.
void ClaimHome(void (*log)(const char* line));

// Brings the app and its window to the front. UI thread.
void ActivateApp();

}  // namespace fable2::app
