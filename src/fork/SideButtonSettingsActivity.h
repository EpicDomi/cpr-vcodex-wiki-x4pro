#pragma once

#include "ReaderSideButtons.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Fork: the "Side Button Actions (Reader)" settings page. Mirrors
// HomeButtonSettingsActivity: one row per button gesture, each opening a popup
// with the available actions.
class SideButtonSettingsActivity final : public UiListActivity {
 public:
  SideButtonSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("SideButtonSettings", renderer, input) {}

 private:
  freeink::ui::ListItem rows[readersidebuttons::ROW_COUNT]{};
  OptionPopup optionPopup;

  int listCount() const override { return readersidebuttons::ROW_COUNT; }
  const char* headerTitle() const override { return tr(STR_SIDE_BTN_ACTIONS); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;
};
