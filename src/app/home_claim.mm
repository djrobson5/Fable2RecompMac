// Fable 2: the Home button and the window (see home_claim.h).

#include "app/home_claim.h"

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void (*g_log)(const char*) = nullptr;

void Say(NSString* text) {
  if (g_log)
    g_log([text UTF8String]);
}

// Home only exists on newer pads; Share (Capture) is left to the system.
void Claim(GCController* controller, const char* why) {
  GCControllerButtonInput* home = controller.physicalInputProfile.buttons[GCInputButtonHome];
  if (!home) {
    Say([NSString stringWithFormat:@"%s: %@ has no Home button", why, controller.vendorName ?: @"a controller"]);
    return;
  }
  home.preferredSystemGestureState = GCSystemGestureStateDisabled;
  Say([NSString stringWithFormat:@"%s: Home claimed on %@", why, controller.vendorName ?: @"a controller"]);
}

}  // namespace

void fable2::app::ClaimHome(void (*log)(const char* line)) {
  const char* off = std::getenv("FABLE2_CLAIM_HOME");
  if (off && std::strcmp(off, "0") == 0)
    return;
  g_log = log;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    // Pads connected later (and a reconnect) come through here, on the main queue.
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                    if ([note.object isKindOfClass:[GCController class]])
                                                      Claim(note.object, "connected");
                                                  }];
    for (GCController* controller in [GCController controllers])
      Claim(controller, "at start");
  });
}

void fable2::app::ActivateApp() {
  NSApplication* app = NSApp;
  if (!app)
    return;
  if ([app respondsToSelector:@selector(activate)]) {
    [app performSelector:@selector(activate)];
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [app activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
  }
}
