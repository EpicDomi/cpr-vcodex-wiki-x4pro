#include "WikiPictureActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

WikiPictureActivity::WikiPictureActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::shared_ptr<WikiSession> session, uint32_t image)
    : Activity("WikiPicture", renderer, mappedInput), session_(std::move(session)), image_(image) {}

void WikiPictureActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  areaX_ = 0;
  areaY_ = metrics.topPadding;
  areaW_ = renderer.getScreenWidth();
  areaH_ = renderer.getScreenHeight() - areaY_ - metrics.buttonHintsHeight;
  xwa::ImageRecord rec;
  failed_ = !session_->archive().imageRecord(image_, rec) || rec.width == 0 || rec.height == 0;
  srcW_ = rec.width;
  srcH_ = rec.height;
  {
    RenderLock lock(*this);
    layout();
  }
  requestUpdate();
}

void WikiPictureActivity::onExit() {
  pixmap_.release();
  Activity::onExit();
}

void WikiPictureActivity::layout() {
  if (failed_) return;
  // Fitted: as large as fits. Enlarged: the stored pixels, or twice the
  // fitted size for small pictures.
  const float fit = std::min(static_cast<float>(areaW_) / srcW_, static_cast<float>(areaH_) / srcH_);
  const float scale = zoomed_ ? std::max(1.0f, fit * 2) : fit;
  scaledW_ = std::max(1, static_cast<int>(srcW_ * scale));
  scaledH_ = std::max(1, static_cast<int>(srcH_ * scale));
  viewX_ = std::max(0, std::min(viewX_, scaledW_ - areaW_));
  viewY_ = std::max(0, std::min(viewY_, scaledH_ - areaH_));
  decode();
}

void WikiPictureActivity::decode() {
  pixmap_.release();
  const int w = std::min(areaW_, scaledW_);
  const int h = std::min(areaH_, scaledH_);
  // The visible part in four greys; when that does not fit in RAM, it is
  // drawn while decoding, in black and white.
  direct_ = !wiki::decodePictureRegion(*session_, image_, scaledW_, scaledH_, viewX_, viewY_, w, h, pixmap_);
}

void WikiPictureActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  int tx = 0;
  int ty = 0;
  const bool tapped = mappedInput.hasTouch() && mappedInput.wasScreenTapped(tx, ty);
  if (tapped || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    RenderLock lock(*this);
    zoomed_ = !zoomed_;
    if (zoomed_ && tapped) {
      // Enlarge around the tapped point.
      const float fit = std::min(static_cast<float>(areaW_) / srcW_, static_cast<float>(areaH_) / srcH_);
      const float zoom = std::max(1.0f, fit * 2) / fit;
      const int fx = tx - (areaW_ - static_cast<int>(srcW_ * fit)) / 2;
      const int fy = ty - areaY_ - (areaH_ - static_cast<int>(srcH_ * fit)) / 2;
      viewX_ = static_cast<int>(fx * zoom) - areaW_ / 2;
      viewY_ = static_cast<int>(fy * zoom) - areaH_ / 2;
    } else {
      viewX_ = viewY_ = 0;
    }
    layout();
    requestUpdate();
    return;
  }
  if (!zoomed_) return;
  int dx = 0;
  int dy = 0;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) dy = 1;
  if (swipe == MappedInputManager::SwipeDir::Down) dy = -1;
  if (swipe == MappedInputManager::SwipeDir::Left) dx = 1;
  if (swipe == MappedInputManager::SwipeDir::Right && !mappedInput.wasBackGesture()) dx = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) dx = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) dx = 1;
  const auto turn = ReaderUtils::detectPageTurn(mappedInput);
  if (turn.next) dy = 1;
  if (turn.prev) dy = -1;
  if (dx == 0 && dy == 0) return;
  RenderLock lock(*this);
  const int oldX = viewX_;
  const int oldY = viewY_;
  viewX_ += dx * areaW_ / 2;
  viewY_ += dy * areaH_ / 2;
  layout();
  if (viewX_ != oldX || viewY_ != oldY) requestUpdate();
}

void WikiPictureActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int w = std::min(areaW_, scaledW_);
  const int h = std::min(areaH_, scaledH_);
  const int x = areaX_ + (areaW_ - w) / 2;
  const int y = areaY_ + (areaH_ - h) / 2;
  auto drawPicture = [&]() {
    if (pixmap_.valid()) pixmap_.draw(renderer, x, y);
  };
  if (failed_) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_WIKI_ARTICLE_DAMAGED));
  } else if (direct_) {
    wiki::drawPictureRegion(renderer, *session_, image_, scaledW_, scaledH_, viewX_, viewY_, w, h, x, y);
  } else {
    drawPicture();
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), zoomed_ ? tr(STR_WIKI_FIT) : tr(STR_WIKI_ZOOM),
                                            zoomed_ ? tr(STR_DIR_LEFT) : "", zoomed_ ? tr(STR_DIR_RIGHT) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  if (pixmap_.valid() && SETTINGS.textAntiAliasing && !renderer.isDarkMode()) {
    ReaderUtils::renderAntiAliased(renderer, drawPicture);
  }
}
