#include "SideButtonSettingsActivity.h"

#include <utility>

#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
const char* actionLabel(const uint8_t value) {
  const uint8_t index = value < static_cast<uint8_t>(readersidebuttons::Action::Count) ? value : 0;
  return I18N.get(readersidebuttons::ACTION_LABELS[index]);
}
}  // namespace

bool SideButtonSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SideButtonSettingsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount() || optionPopup.isActive()) return;
  app.clearTapFlash();
  nav.selected = index;
  const auto field = readersidebuttons::FIELDS[index];
  const uint8_t value = SETTINGS.*field;
  const int current = value < static_cast<uint8_t>(readersidebuttons::Action::Count) ? value : 0;
  optionPopup.show(readersidebuttons::ROW_LABELS[index], readersidebuttons::ACTION_LABELS,
                   static_cast<int>(readersidebuttons::Action::Count), current, [field](int selected) {
                     SETTINGS.*field = static_cast<uint8_t>(selected);
                     SETTINGS.saveToFile();
                   });
  requestUpdate();
}

void SideButtonSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void SideButtonSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  for (int i = 0; i < listCount(); ++i) {
    rows[i].label = I18N.get(readersidebuttons::ROW_LABELS[i]);
    rows[i].actionValue = static_cast<int16_t>(i);
    rows[i].value = actionLabel(SETTINGS.*readersidebuttons::FIELDS[i]);
  }
  fui::ListProps props;
  props.items = rows;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
