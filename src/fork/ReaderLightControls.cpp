#include "ReaderLightControls.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"

namespace readerlight {
namespace {

enum class Side : uint8_t { None, Left, Right };

// Lowest level a slide can reach; matches the frontlight panel's slider floor.
constexpr int MIN_BRIGHTNESS = 1;
// After a slide ends, keep swallowing reader input briefly so the finger lift
// cannot also register as a tap, swipe or Back.
constexpr unsigned long RELEASE_GUARD_MS = 200;

struct SlideState {
  bool tracking = false;  // a finger is down and this contact has been classified
  bool active = false;    // the contact qualified as a light slide
  bool swallow = false;   // slide ended early; ignore the rest of this contact
  bool changed = false;
  Side side = Side::None;
  int startX = 0;
  int startY = 0;
  int initialValue = 0;
  unsigned long guardUntilMs = 0;
};

SlideState state;

// The slide must start inside this strip along the edge...
int startBand(const int screenWidth) { return std::max(40, screenWidth * 10 / 100); }
// ...and may drift this far inward before it is abandoned.
int driftBand(const int screenWidth) { return std::max(80, screenWidth * 22 / 100); }
// Top and bottom strips are left to the edge gestures that begin there (light
// panel, menu, Home).
int verticalMargin(const int screenHeight) { return screenHeight * 12 / 100; }
int minTravel(const int screenHeight) { return std::max(60, screenHeight * 6 / 100); }

// CrossInk's scale: the first qualifying movement changes one point and a
// full-height slide covers the whole 0-100 range.
int amountFor(const int distance, const int screenHeight) {
  if (screenHeight < 2) return 0;
  const int minimum = minTravel(screenHeight);
  if (distance < minimum) return 0;
  const int maximum = screenHeight - 1;
  const int travel = std::max(1, maximum - minimum);
  return 1 + (std::min(distance, maximum) - minimum) * 99 / travel;
}

bool insideBand(const Side side, const int x, const int screenWidth, const int band) {
  return side == Side::Left ? x < band : x >= screenWidth - band;
}

void apply(const int y, const int screenHeight) {
  const int dy = state.startY - y;  // up = positive
  const int amount = amountFor(std::abs(dy), screenHeight);
  const int signedAmount = dy >= 0 ? amount : -amount;
  if (state.side == Side::Left) {
    const int target = std::clamp(state.initialValue + signedAmount, MIN_BRIGHTNESS, 100);
    if (target != Frontlight.brightness()) {
      Frontlight.setBrightness(static_cast<uint8_t>(target));
      SETTINGS.frontlightBrightness = static_cast<uint8_t>(target);
      state.changed = true;
    }
    // Raising the level with the light off switches it on, like the panel slider.
    if (signedAmount > 0 && !Frontlight.isOn()) {
      Frontlight.setOn(true);
      SETTINGS.frontlightOn = 1;
      state.changed = true;
    }
  } else {
    const int target = std::clamp(state.initialValue + signedAmount, 0, 100);
    if (target != Frontlight.warmth()) {
      Frontlight.setWarmth(static_cast<uint8_t>(target));
      SETTINGS.frontlightWarmth = static_cast<uint8_t>(target);
      state.changed = true;
    }
  }
}

// Ends the current slide. Returns true when a slide was in progress.
bool finish() {
  const bool wasActive = state.active;
  if (wasActive) {
    if (state.changed) SETTINGS.saveToFile();
    LOG_INF("LIGHT", "Edge slide done: brightness=%u warmth=%u", Frontlight.brightness(), Frontlight.warmth());
    state.guardUntilMs = millis() + RELEASE_GUARD_MS;
  }
  state.active = false;
  state.changed = false;
  state.side = Side::None;
  return wasActive;
}

}  // namespace

bool handleEdgeSlide(const MappedInputManager& input, const GfxRenderer& renderer) {
  if (!SETTINGS.readerEdgeLightSlide || !Frontlight.present() || !input.hasTouch()) {
    if (state.tracking) finish();
    state.tracking = false;
    state.swallow = false;
    return false;
  }

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  int x = 0;
  int y = 0;
  if (!input.isScreenTouchHeld(x, y)) {
    // Finger lifted (or never down): close out the contact.
    bool ended = false;
    if (state.tracking) {
      ended = finish();
      if (state.swallow) {
        ended = true;
        state.guardUntilMs = millis() + RELEASE_GUARD_MS;
      }
    }
    state.tracking = false;
    state.swallow = false;
    if (ended) return true;
    if (state.guardUntilMs != 0) {
      if (static_cast<long>(state.guardUntilMs - millis()) > 0) return true;
      state.guardUntilMs = 0;
    }
    return false;
  }

  if (!state.tracking) {
    // New contact: decide once whether it started in a slide strip.
    state.tracking = true;
    state.active = false;
    state.swallow = false;
    state.changed = false;
    state.guardUntilMs = 0;
    state.startX = x;
    state.startY = y;
    const int band = startBand(width);
    const int margin = verticalMargin(height);
    const bool verticalOk = y >= margin && y < height - margin;
    if (verticalOk && x < band) {
      state.side = Side::Left;
    } else if (verticalOk && x >= width - band && Frontlight.hasColorTemperature()) {
      state.side = Side::Right;
    } else {
      state.side = Side::None;
    }
    return false;
  }

  if (state.swallow) return true;
  if (state.side == Side::None) return false;

  if (!insideBand(state.side, x, width, driftBand(width))) {
    // Drifted inward: before qualifying this is some other gesture (Back,
    // page swipe). After qualifying, the slide ends at its last value and the
    // rest of the contact is ignored so its lift cannot act as Back or a swipe.
    if (!finish()) return false;
    state.swallow = true;
    return true;
  }

  if (!state.active) {
    const int dx = std::abs(x - state.startX);
    const int dy = std::abs(y - state.startY);
    if (dy < minTravel(height) || dy * 2 < dx * 3) return false;
    state.active = true;
    state.initialValue = state.side == Side::Left ? Frontlight.brightness() : Frontlight.warmth();
  }

  apply(y, height);
  return true;
}

}  // namespace readerlight
