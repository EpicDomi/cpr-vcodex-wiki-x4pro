#include "WikiListActivity.h"

#include <I18n.h>

#include "components/UITheme.h"

namespace fui = freeink::ui;

WikiListActivity::WikiListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                   std::vector<WikiRow> rows, int selected, MoreFn more)
    : UiListActivity("WikiList", renderer, mappedInput),
      title_(std::move(title)),
      rows_(std::move(rows)),
      initial_(selected),
      more_(std::move(more)) {
  exhausted_ = !more_;
}

void WikiListActivity::onEnter() {
  UiListActivity::onEnter();
  if (rows_.empty()) fetchMore();
  rebuild();
  nav.reset(std::max(0, std::min(initial_, listCount() - 1)));
}

void WikiListActivity::rebuild() {
  items_.resize(rows_.size());
  for (size_t i = 0; i < rows_.size(); i++) {
    fui::ListItem& it = items_[i];
    it = fui::ListItem{};
    it.label = rows_[i].label.c_str();
    it.subtitle = rows_[i].subtitle.empty() ? nullptr : rows_[i].subtitle.c_str();
    it.value = rows_[i].value.empty() ? nullptr : rows_[i].value.c_str();
    it.isHeader = rows_[i].header;
    it.actionValue = static_cast<int16_t>(i);
  }
}

void WikiListActivity::fetchMore() {
  if (exhausted_ || !more_) return;
  if (!more_(rows_)) exhausted_ = true;
}

bool WikiListActivity::handleCustomInput() {
  // Read further rows before the reader reaches the end of what is loaded.
  if (!exhausted_ && listCount() - nav.selected <= 8) {
    const size_t before = rows_.size();
    fetchMore();
    if (rows_.size() != before) {
      rebuild();
      requestUpdate();
    }
  }
  return false;
}

void WikiListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText.maxLines = 3;
  syncListViewport(screen, props);
  screen.list(props);
}

void WikiListActivity::activateIndex(int index) {
  if (index < 0 || index >= static_cast<int>(rows_.size()) || rows_[index].id < 0) return;
  app.clearTapFlash();
  setResult(IntervalResult{static_cast<uint32_t>(rows_[index].id)});
  finish();
}

void WikiListActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
