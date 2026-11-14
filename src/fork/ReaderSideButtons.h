#pragma once

#include <I18n.h>

#include <cstdint>

#include "CrossPointSettings.h"

// Fork: per-button actions for the two side buttons while a book page is on
// screen. "Left" is BTN_UP (stock: previous page) and "Right" is BTN_DOWN
// (stock: next page). Each has a tap, double-tap and long-press action, in the
// style of the Home button gestures. With every row on Default the module is
// inert and the stock page-turn handling runs untouched.
namespace readersidebuttons {

// Persisted indices: append actions without reordering existing values.
enum class Action : uint8_t {
  Default,  // tap / long press: the button's stock page function; double tap: nothing
  Ignore,
  PreviousPage,
  NextPage,
  ToggleFrontlight,
  ReaderMenu,
  Bookmark,
  Refresh,
  Dictionary,
  Footnotes,
  Home,
  Count
};

inline constexpr int ROW_COUNT = 6;  // left tap, double, long; right tap, double, long
inline constexpr StrId ACTION_LABELS[] = {
    StrId::STR_DEFAULT_VALUE,     StrId::STR_IGNORE,      StrId::STR_PREV_PAGE_GESTURE, StrId::STR_NEXT_PAGE_GESTURE,
    StrId::STR_TOGGLE_FRONTLIGHT, StrId::STR_READER_MENU, StrId::STR_BOOKMARK_OPTION,   StrId::STR_FORCE_REFRESH,
    StrId::STR_DICTIONARY,        StrId::STR_FOOTNOTES,   StrId::STR_HOME_SHORTCUT};
static_assert(sizeof(ACTION_LABELS) / sizeof(ACTION_LABELS[0]) == static_cast<unsigned>(Action::Count));
inline constexpr StrId ROW_LABELS[ROW_COUNT] = {StrId::STR_SIDE_LEFT_TAP,         StrId::STR_SIDE_LEFT_DOUBLE_TAP,
                                                StrId::STR_SIDE_LEFT_LONG_PRESS,  StrId::STR_SIDE_RIGHT_TAP,
                                                StrId::STR_SIDE_RIGHT_DOUBLE_TAP, StrId::STR_SIDE_RIGHT_LONG_PRESS};
inline constexpr uint8_t CrossPointSettings::* FIELDS[ROW_COUNT] = {
    &CrossPointSettings::sideLeftTapAction,        &CrossPointSettings::sideLeftDoubleTapAction,
    &CrossPointSettings::sideLeftLongPressAction,  &CrossPointSettings::sideRightTapAction,
    &CrossPointSettings::sideRightDoubleTapAction, &CrossPointSettings::sideRightLongPressAction};
inline constexpr const char* KEYS[ROW_COUNT] = {"sideLeftTapAction",        "sideLeftDoubleTapAction",
                                                "sideLeftLongPressAction",  "sideRightTapAction",
                                                "sideRightDoubleTapAction", "sideRightLongPressAction"};

// Page-turn requests the side buttons produced this pass.
struct PageEvents {
  bool handled = false;       // the module owns both side buttons; skip the stock mapping
  bool leftDefault = false;   // left button asked for its stock page function
  bool rightDefault = false;  // right button asked for its stock page function
  bool prev = false;
  bool next = false;
};

// Called by the reader's page view each pass (ReaderUtils::detectPageTurn).
// Also keeps the module armed; it disarms shortly after the calls stop.
PageEvents pageEvents();
// Turns the events into previous/next using the Side Button Layout setting.
void resolve(const PageEvents& events, bool navDirectionSwapped, bool& prev, bool& next);
// Any screen change ends the page view (Activity::onEnter).
void deactivate();

}  // namespace readersidebuttons
