#pragma once

// Fork diagnostic: writes the perflog timeline to
// /.crosspoint/cpr-vcodex-logs/timing.log while the device is idle. Controlled
// by SETTINGS.timingLog; does nothing when it is off.
namespace perflogwriter {

// Call once per main-loop pass. lastInputMs is the millis() of the last user input.
void tick(unsigned long lastInputMs);

}  // namespace perflogwriter
