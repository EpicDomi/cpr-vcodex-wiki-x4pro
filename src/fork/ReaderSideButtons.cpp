#include "ReaderSideButtons.h"

#include <Arduino.h>
#include <HalGPIO.h>

#include "MappedInputManager.h"
#include "util/HomeButtonInput.h"

namespace readersidebuttons {
namespace {

constexpr unsigned long LONG_PRESS_MS = 700;  // ReaderUtils::SKIP_HOLD_MS
constexpr unsigned long DOUBLE_TAP_MS = HomeButtonInput::DOUBLE_TAP_MS;
// The page view re-arms the module every pass; a gap this long means the reader
// is showing something else (an overlay) and the buttons revert to stock.
constexpr unsigned long PAGE_VIEW_WINDOW_MS = 1000;

struct ButtonState {
  bool down = false;        // a press this module is tracking
  bool consumed = false;    // the press already acted (on press or long press)
  bool pendingTap = false;  // first tap waiting for a possible second
  bool second = false;      // the second press of a double tap is in progress
  unsigned long pressedAt = 0;
  unsigned long pendingAt = 0;
};

ButtonState states[2];
PageEvents frame;
bool pageViewSeen = false;
unsigned long lastPageViewMs = 0;

Action actionAt(const int row) {
  const uint8_t value = SETTINGS.*FIELDS[row];
  return value < static_cast<uint8_t>(Action::Count) ? static_cast<Action>(value) : Action::Default;
}

bool isNone(const Action action) { return action == Action::Default || action == Action::Ignore; }

bool customised(const int button) {
  return actionAt(button * 3) != Action::Default || !isNone(actionAt(button * 3 + 1)) ||
         actionAt(button * 3 + 2) != Action::Default;
}

struct Outcome {
  HomeButtonAction homeAction = HomeButtonAction::Ignore;
  bool shortPageEvent = false;  // a page event that must not read as a long press
};

void fire(const int button, const Action action, const bool isLong, Outcome& outcome) {
  switch (action) {
    case Action::Default:
      (button == 0 ? frame.leftDefault : frame.rightDefault) = true;
      if (!isLong) outcome.shortPageEvent = true;
      break;
    case Action::PreviousPage:
      frame.prev = true;
      if (!isLong) outcome.shortPageEvent = true;
      break;
    case Action::NextPage:
      frame.next = true;
      if (!isLong) outcome.shortPageEvent = true;
      break;
    case Action::ToggleFrontlight:
      outcome.homeAction = HomeButtonAction::ToggleFrontlight;
      break;
    case Action::ReaderMenu:
      outcome.homeAction = HomeButtonAction::ReaderMenu;
      break;
    case Action::Bookmark:
      outcome.homeAction = HomeButtonAction::Bookmark;
      break;
    case Action::Refresh:
      outcome.homeAction = HomeButtonAction::Refresh;
      break;
    case Action::Dictionary:
      outcome.homeAction = HomeButtonAction::Dictionary;
      break;
    case Action::Footnotes:
      outcome.homeAction = HomeButtonAction::Footnotes;
      break;
    case Action::Home:
      outcome.homeAction = HomeButtonAction::Home;
      break;
    case Action::Ignore:
    case Action::Count:
      break;
  }
}

Outcome poll(const HalGPIO& gpio) {
  Outcome outcome;
  frame = {};
  const unsigned long now = millis();
  const bool armed = pageViewSeen && now - lastPageViewMs <= PAGE_VIEW_WINDOW_MS && (customised(0) || customised(1));
  if (!armed) {
    states[0] = {};
    states[1] = {};
    return outcome;
  }
  frame.handled = true;
  const bool actOnPress = SETTINGS.longPressButtonBehavior == CrossPointSettings::LONG_PRESS_OFF;

  for (int button = 0; button < 2; ++button) {
    const uint8_t id = button == 0 ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
    const Action tap = actionAt(button * 3);
    const Action doubleTap = actionAt(button * 3 + 1);
    const Action longPress = actionAt(button * 3 + 2);
    const bool hasDouble = !isNone(doubleTap);
    ButtonState& state = states[button];

    if (gpio.wasPressed(id)) {
      state.down = true;
      state.consumed = false;
      state.pressedAt = now;
      state.second = state.pendingTap && now - state.pendingAt <= DOUBLE_TAP_MS;
      // An untouched button keeps the stock press-to-turn response when long
      // press is switched off.
      if (!state.second && actOnPress && !customised(button)) {
        state.consumed = true;
        fire(button, tap, false, outcome);
      }
    }

    if (state.down && !state.consumed && gpio.isPressed(id) && now - state.pressedAt >= LONG_PRESS_MS) {
      state.consumed = true;
      state.pendingTap = false;
      state.second = false;
      fire(button, longPress, true, outcome);
    }

    if (gpio.wasReleased(id) && state.down) {
      state.down = false;
      if (!state.consumed) {
        if (state.second) {
          state.second = false;
          state.pendingTap = false;
          fire(button, doubleTap, false, outcome);
        } else if (hasDouble) {
          state.pendingTap = true;
          state.pendingAt = now;
        } else {
          fire(button, tap, false, outcome);
        }
      }
      state.consumed = false;
    }

    if (state.pendingTap && !state.second && now - state.pendingAt > DOUBLE_TAP_MS) {
      state.pendingTap = false;
      fire(button, tap, false, outcome);
    }
  }
  return outcome;
}

}  // namespace

PageEvents pageEvents() {
  pageViewSeen = true;
  lastPageViewMs = millis();
  return frame;
}

void resolve(const PageEvents& events, const bool navDirectionSwapped, bool& prev, bool& next) {
  prev = events.prev;
  next = events.next;
  // Same mapping as MappedInputManager's PageBack / PageForward.
  const auto apply = [&](const bool isLeft) {
    switch (SETTINGS.sideButtonLayout) {
      case CrossPointSettings::PREV_NEXT:
        (isLeft != navDirectionSwapped ? prev : next) = true;
        break;
      case CrossPointSettings::NEXT_PREV:
        (isLeft != navDirectionSwapped ? next : prev) = true;
        break;
      case CrossPointSettings::PREV_PREV:
        prev = true;
        break;
      case CrossPointSettings::NEXT_NEXT:
        next = true;
        break;
      default:
        break;
    }
  };
  if (events.leftDefault) apply(true);
  if (events.rightDefault) apply(false);
}

void deactivate() {
  pageViewSeen = false;
  frame = {};
  states[0] = {};
  states[1] = {};
}

}  // namespace readersidebuttons

// Runs right after update() in the main loop. Non-page actions travel through
// the Home-key action channel, so the reader, the main loop and the activity
// manager handle them exactly as they do for the Home key.
void MappedInputManager::applyReaderSideButtons() const {
  const auto outcome = readersidebuttons::poll(gpio);
  if (homeAction == HomeButtonAction::Ignore) homeAction = outcome.homeAction;
  // A tap that fires after the double-tap wait must not be mistaken for a hold.
  if (outcome.shortPageEvent) sampledButtonHeldMs = 0;
}
