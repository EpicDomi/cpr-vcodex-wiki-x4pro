#include "WikiReaderActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "WikiListActivity.h"
#include "WikiPictureActivity.h"
#include "WikiTableActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "fork/ReaderLightControls.h"

namespace {
constexpr const char* TAG = "WIKI";
}

WikiReaderActivity::WikiReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::shared_ptr<WikiSession> session, Target start, std::vector<WikiPlace> trail)
    : Activity("WikiReader", renderer, mappedInput),
      session_(std::move(session)),
      start_(start),
      trail_(std::move(trail)) {}

WikiReaderActivity::~WikiReaderActivity() = default;

void WikiReaderActivity::onEnter() {
  Activity::onEnter();
  pagesUntilFullRefresh_ = SETTINGS.getRefreshFrequency();
  computeViewport();
  // Hyphenation patterns for the archive's language.
  Hyphenator::setPreferredLanguage(session_->archive().metaValue("language"));
  RenderLock lock(*this);
  open(start_);
  ReaderUtils::requestReaderUiTransitionRefresh(renderer);
  requestUpdate();
}

void WikiReaderActivity::onExit() {
  remember();
  pixmaps_.clear();
  layout_.reset();
  Activity::onExit();
}

void WikiReaderActivity::computeViewport() {
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.screenMargin;
  left += SETTINGS.screenMargin;
  right += SETTINGS.screenMargin;
  bottom += std::max<int>(SETTINGS.screenMargin, UITheme::getInstance().getStatusBarHeight());
  marginTop_ = top;
  marginLeft_ = left;
  viewW_ = renderer.getScreenWidth() - left - right;
  viewH_ = renderer.getScreenHeight() - top - bottom;
  style_ = wiki::TextStyle::fromSettings(renderer);
}

WikiPlace WikiReaderActivity::here() const {
  WikiPlace p;
  p.article = article_;
  if (layout_ && pageIndex_ < static_cast<int>(pages_.size())) {
    p.pos = layout_->bodyPos(pages_[pageIndex_]);
    p.line = pages_[pageIndex_].line;
  }
  return p;
}

void WikiReaderActivity::remember() {
  if (article_ == xwa::NO_ARTICLE || failed_) return;
  WikiMemory& m = session_->memory();
  m.resume = here();
  m.trail = trail_;
  session_->saveMemory();
}

bool WikiReaderActivity::open(const Target& target) {
  failed_ = false;
  pixmaps_.clear();
  pixmapsReady_ = false;
  page_.clear();
  layout_.reset();
  article_ = target.place.article;
  xwa::StreamReader r;
  if (!r.open(session_->archive(), article_) || !xwa::readHeader(r, header_)) {
    LOG_ERR(TAG, "Article %u could not be read", static_cast<unsigned>(article_));
    failed_ = true;
    return false;
  }
  // Where to start: a saved place, a section, or a named place.
  uint32_t pos = target.place.pos;
  uint32_t line = target.place.line;
  if (target.byAnchor || target.bySection) {
    int section = static_cast<int>(target.place.pos);
    if (target.byAnchor) section = std::max(0, xwa::findPlace(header_, target.place.pos));
    pos = section > 0 && section <= static_cast<int>(header_.toc.size()) ? header_.toc[section - 1].pos : 0;
    line = 0;
  }
  layout_.reset(new (std::nothrow) wiki::Layout(renderer, *session_, article_, header_, style_,
                                                static_cast<int16_t>(viewW_), static_cast<int16_t>(viewH_)));
  if (!layout_) {
    failed_ = true;
    return false;
  }
  pages_.clear();
  pages_.push_back(layout_->first());
  counted_ = false;
  // Lay out pages up to the one holding the place; the rest are counted
  // while the reader reads.
  int index = 0;
  for (;;) {
    wiki::Page tmp;
    wiki::Cursor next;
    if (!layout_->page(pages_.back(), tmp, next) || next.end) {
      counted_ = true;
      break;
    }
    const uint32_t nextPos = layout_->bodyPos(next);
    if (nextPos > pos || (nextPos == pos && next.line > line)) break;
    pages_.push_back(next);
    index = static_cast<int>(pages_.size()) - 1;
  }
  session_->noteRead(article_);
  showPage(index);
  return !failed_;
}

void WikiReaderActivity::showPage(int index) {
  pageIndex_ = std::max(0, std::min(index, static_cast<int>(pages_.size()) - 1));
  pixmaps_.clear();
  pixmapsReady_ = false;
  wiki::Cursor next;
  if (!layout_ || !layout_->page(pages_[pageIndex_], page_, next)) {
    failed_ = true;
    return;
  }
  // The page's end is the next page's start: record it if it is new.
  if (pageIndex_ + 1 == static_cast<int>(pages_.size())) {
    if (next.end) {
      counted_ = true;
    } else {
      pages_.push_back(next);
    }
  }
}

bool WikiReaderActivity::paginateOne() {
  if (counted_ || !layout_) return false;
  wiki::Page tmp;
  wiki::Cursor next;
  if (!layout_->page(pages_.back(), tmp, next) || next.end) {
    counted_ = true;
  } else {
    pages_.push_back(next);
  }
  return true;
}

bool WikiReaderActivity::turn(int delta) {
  if (failed_ || !layout_) return false;
  const int target = pageIndex_ + delta;
  if (target < 0 || target >= static_cast<int>(pages_.size())) return false;
  showPage(target);
  return true;
}

void WikiReaderActivity::follow(uint32_t article, uint32_t place, bool anchored) {
  RenderLock lock(*this);
  trail_.push_back(here());
  if (trail_.size() > WikiMemory::MAX_TRAIL) trail_.erase(trail_.begin());
  Target t;
  t.place.article = article;
  t.place.pos = place;
  t.byAnchor = anchored;
  t.bySection = !anchored;
  open(t);
  requestUpdate();
}

void WikiReaderActivity::goBack() {
  if (trail_.empty()) {
    finish();
    return;
  }
  RenderLock lock(*this);
  Target t;
  t.place = trail_.back();
  trail_.pop_back();
  open(t);
  requestUpdate();
}

// ------------------------------------------------------------------ input

bool WikiReaderActivity::handleTap(int x, int y) {
  const int px = x - marginLeft_;
  const int py = y - marginTop_;
  const wiki::HitArea* best = nullptr;
  for (const auto& a : page_.areas) {
    // A little slack around small targets for fingers.
    const int slack = a.kind == wiki::HitArea::Link ? 4 : 0;
    if (px >= a.x - slack && px < a.x + a.w + slack && py >= a.y && py < a.y + a.h) {
      best = &a;
      break;
    }
  }
  if (!best) return false;
  switch (best->kind) {
    case wiki::HitArea::Link:
      follow(best->target, best->place, best->anchored);
      return true;
    case wiki::HitArea::Table:
      openTable(best->target);
      return true;
    case wiki::HitArea::Picture:
      openPicture(best->target);
      return true;
  }
  return false;
}

void WikiReaderActivity::loop() {
  if (readerlight::handleEdgeSlide(mappedInput, renderer)) return;

  switch (mappedInput.homeButtonAction()) {
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Confirm:
      openMenu();
      return;
    case HomeButtonAction::Footnotes:
      openLinks();
      return;
    case HomeButtonAction::Refresh:
      forceFullRefresh_ = true;
      requestUpdate();
      return;
    default:
      break;
  }

  // Back: the previous article, or out of the reader. Held: out at once.
  if (!mappedInput.wasBackGesture()) {
    if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, ReaderUtils::GO_BACK_OR_HOME_MS)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
      return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openMenu();
    return;
  }

  // A tap on a link, table or picture comes before page turns and the menu.
  int tx = 0;
  int ty = 0;
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(tx, ty) && handleTap(tx, ty)) return;

  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput) || ReaderUtils::isTouchMenuTap(renderer, mappedInput)) {
    openMenu();
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prev, next, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromTilt;
  prev = prev || touch.prev;
  next = next || touch.next;
  if (prev || next) {
    RenderLock lock(*this);
    if (turn(next ? 1 : -1)) requestUpdate();
    return;
  }

  // Idle: count one more page, so the page total is known soon.
  if (!counted_) {
    RenderLock lock(*this);
    paginateOne();
  }
}

// ------------------------------------------------------------------ menus

void WikiReaderActivity::openMenu() {
  std::vector<WikiRow> rows;
  auto add = [&rows](const char* label, int64_t id, std::string subtitle = {}) {
    WikiRow r;
    r.label = label;
    r.subtitle = std::move(subtitle);
    r.id = id;
    rows.push_back(std::move(r));
  };
  if (!header_.toc.empty()) add(tr(STR_WIKI_CONTENTS), ACT_CONTENTS);
  bool links = false;
  for (const auto& a : page_.areas) links = links || a.kind == wiki::HitArea::Link;
  if (links) add(tr(STR_WIKI_LINKS), ACT_LINKS);
  add(tr(STR_WIKI_SEARCH), ACT_SEARCH);
  if (!trail_.empty()) {
    std::string title;
    session_->archive().title(trail_.back().article, title);
    add(tr(STR_WIKI_PREVIOUS), ACT_BACK, title);
  }
  add(tr(STR_WIKI_RECENT), ACT_RECENT);
  add(tr(STR_WIKI_RANDOM), ACT_RANDOM);
  add(tr(STR_WIKI_HOME), ACT_HOME);
  startActivityForResult(std::make_unique<WikiListActivity>(renderer, mappedInput, header_.title, std::move(rows)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* chosen = std::get_if<IntervalResult>(&result.data);
                           if (!chosen) return;
                           switch (static_cast<int64_t>(chosen->value)) {
                             case ACT_CONTENTS:
                               openContents();
                               break;
                             case ACT_LINKS:
                               openLinks();
                               break;
                             case ACT_SEARCH:
                               openSearch();
                               break;
                             case ACT_BACK:
                               goBack();
                               break;
                             case ACT_RECENT:
                               openRecent();
                               break;
                             case ACT_RANDOM: {
                               const uint32_t n = session_->archive().articleCount();
                               if (n > 0) follow(static_cast<uint32_t>(random(static_cast<long>(n))), 0, false);
                               break;
                             }
                             case ACT_HOME:
                               finish();
                               break;
                             default:
                               break;
                           }
                         });
}

void WikiReaderActivity::openContents() {
  std::vector<WikiRow> rows;
  WikiRow top;
  top.label = header_.title;
  top.id = 0;
  rows.push_back(top);
  int current = 0;
  const uint32_t pos = page_.startPos;
  for (size_t i = 0; i < header_.toc.size(); i++) {
    const auto& e = header_.toc[i];
    WikiRow r;
    // Subsections indented under their sections.
    const int depth = std::max(0, e.level - 2);
    r.label = std::string(static_cast<size_t>(depth) * 3, ' ') + e.text;
    r.id = static_cast<int64_t>(i + 1);
    rows.push_back(std::move(r));
    if (e.pos <= pos) current = static_cast<int>(i + 1);
  }
  startActivityForResult(
      std::make_unique<WikiListActivity>(renderer, mappedInput, tr(STR_WIKI_CONTENTS), std::move(rows), current),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* chosen = std::get_if<IntervalResult>(&result.data);
        if (!chosen) return;
        RenderLock lock(*this);
        const uint32_t section = chosen->value;
        const uint32_t pos = section > 0 && section <= header_.toc.size() ? header_.toc[section - 1].pos : 0;
        // The page holding the section: count pages up to it.
        int index = 0;
        for (size_t i = 0;; i++) {
          if (i + 1 >= pages_.size() && !paginateOne()) break;
          if (i + 1 >= pages_.size()) break;
          if (layout_->bodyPos(pages_[i + 1]) > pos) break;
          index = static_cast<int>(i + 1);
        }
        showPage(index);
      });
}

void WikiReaderActivity::openLinks() {
  std::vector<WikiRow> rows;
  std::vector<const wiki::HitArea*> targets;
  uint32_t lastGroup = UINT32_MAX;
  for (const auto& a : page_.areas) {
    if (a.kind != wiki::HitArea::Link || a.group == lastGroup) continue;
    lastGroup = a.group;
    WikiRow r;
    std::string title;
    session_->archive().title(a.target, title);
    if (a.target == article_ && !a.anchored && a.place > 0 && a.place <= header_.toc.size()) {
      title = header_.toc[a.place - 1].text;
    }
    r.label = title;
    r.id = static_cast<int64_t>(targets.size());
    targets.push_back(&a);
    rows.push_back(std::move(r));
  }
  if (rows.empty()) return;
  // Copies: the page may be laid out again before the result arrives.
  std::vector<wiki::HitArea> chosen;
  for (const auto* t : targets) chosen.push_back(*t);
  startActivityForResult(std::make_unique<WikiListActivity>(renderer, mappedInput, tr(STR_WIKI_LINKS), std::move(rows)),
                         [this, chosen](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* pick = std::get_if<IntervalResult>(&result.data);
                           if (!pick || pick->value >= chosen.size()) return;
                           const auto& a = chosen[pick->value];
                           follow(a.target, a.place, a.anchored);
                         });
}

void WikiReaderActivity::openSearch() {
  if (!search_) search_.reset(new (std::nothrow) WikiSearch(session_));
  if (!search_) return;
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, session_->title(), "", 0);
  keyboard->setSuggestions(search_.get());
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* kb = std::get_if<KeyboardResult>(&result.data);
    if (!kb) return;
    if (kb->suggestion >= 0 && kb->suggestion < search_->count()) {
      const xwa::Hit h = search_->hit(kb->suggestion);
      follow(h.article, h.hasAnchor() ? h.anchor : 0, h.hasAnchor());
      return;
    }
    if (kb->text.empty()) return;
    std::vector<WikiRow> rows;
    search_->listAll(kb->text, rows, listHits_);
    startActivityForResult(std::make_unique<WikiListActivity>(
                               renderer, mappedInput, kb->text, std::move(rows), 1,
                               [this](std::vector<WikiRow>& more) { return search_->listMore(more, listHits_); }),
                           [this](const ActivityResult& r) {
                             if (r.isCancelled) return;
                             const auto* pick = std::get_if<IntervalResult>(&r.data);
                             if (!pick || pick->value >= listHits_.size()) return;
                             const xwa::Hit h = listHits_[pick->value];
                             follow(h.article, h.hasAnchor() ? h.anchor : 0, h.hasAnchor());
                           });
  });
}

void WikiReaderActivity::openRecent() {
  std::vector<WikiRow> rows;
  for (const uint32_t a : session_->memory().recent) {
    WikiRow r;
    if (!session_->archive().title(a, r.label)) continue;
    r.id = a;
    rows.push_back(std::move(r));
  }
  startActivityForResult(
      std::make_unique<WikiListActivity>(renderer, mappedInput, tr(STR_WIKI_RECENT), std::move(rows)),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        if (const auto* pick = std::get_if<IntervalResult>(&result.data)) follow(pick->value, 0, false);
      });
}

void WikiReaderActivity::openTable(uint32_t index) {
  startActivityForResult(std::make_unique<WikiTableActivity>(renderer, mappedInput, session_, article_, index),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           if (const auto* link = std::get_if<WikiLinkResult>(&result.data)) {
                             follow(link->article, link->place, link->anchored);
                           }
                         });
}

void WikiReaderActivity::openPicture(uint32_t image) {
  startActivityForResult(std::make_unique<WikiPictureActivity>(renderer, mappedInput, session_, image),
                         [](const ActivityResult&) {});
}

// ------------------------------------------------------------------ drawing

void WikiReaderActivity::preparePixmaps() {
  if (pixmapsReady_) return;
  pixmapsReady_ = true;
  pixmaps_.clear();
  pixmaps_.resize(page_.items.size());
  for (size_t i = 0; i < page_.items.size(); i++) {
    const wiki::DrawItem& it = page_.items[i];
    if (it.kind == wiki::DrawItem::Image) {
      wiki::decodePicture(*session_, it.a, it.w, it.h, pixmaps_[i]);
    } else if (it.kind == wiki::DrawItem::Formula) {
      wiki::renderFormula(*session_, article_, it.a, it.b, it.w, it.h, pixmaps_[i]);
    }
  }
}

void WikiReaderActivity::drawPage() const {
  const int ox = marginLeft_;
  const int oy = marginTop_;
  for (size_t i = 0; i < page_.items.size(); i++) {
    const wiki::DrawItem& it = page_.items[i];
    switch (it.kind) {
      case wiki::DrawItem::Text:
        renderer.drawText(style_.id[it.font], ox + it.x, oy + it.y, page_.text.c_str() + it.a, true,
                          static_cast<EpdFontFamily::Style>(it.style), BidiUtils::BidiBaseDir::AUTO, style_.tracking);
        break;
      case wiki::DrawItem::Fill:
        renderer.fillRect(ox + it.x, oy + it.y, it.w, it.h, true);
        break;
      case wiki::DrawItem::Frame:
        renderer.drawRect(ox + it.x, oy + it.y, it.w, it.h, true);
        break;
      case wiki::DrawItem::Image:
      case wiki::DrawItem::Formula:
        if (i < pixmaps_.size() && pixmaps_[i].valid()) {
          pixmaps_[i].draw(renderer, ox + it.x, oy + it.y);
        } else if (it.kind == wiki::DrawItem::Image) {
          // A picture that could not be shown keeps its place.
          renderer.drawRect(ox + it.x, oy + it.y, it.w, it.h, true);
        }
        break;
    }
  }
}

void WikiReaderActivity::drawStatusBar() const {
  int total = static_cast<int>(pages_.size());
  if (!counted_ && page_.endPos > 0 && header_.bodySize > page_.endPos) {
    // Until every page is counted: pages so far, scaled by how much of the
    // article they hold.
    const int64_t estimate =
        (static_cast<int64_t>(pageIndex_ + 1) * header_.bodySize + page_.endPos - 1) / page_.endPos;
    total = std::max<int>(total, static_cast<int>(std::min<int64_t>(estimate, 9999)));
  }
  const float progress = header_.bodySize ? 100.0f * page_.endPos / header_.bodySize : 100.0f;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) title = header_.title;
  GUI.drawStatusBar(renderer, std::min(100.0f, progress), pageIndex_ + 1, std::max(total, pageIndex_ + 1), title, 0, 0,
                    true, false, !counted_);
}

void WikiReaderActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (failed_ || !layout_) {
    const int y = renderer.getScreenHeight() / 2 - renderer.getLineHeight(UI_12_FONT_ID);
    const auto lines =
        renderer.wrappedText(UI_12_FONT_ID, tr(STR_WIKI_ARTICLE_DAMAGED), renderer.getScreenWidth() - 40, 4);
    for (size_t i = 0; i < lines.size(); i++) {
      renderer.drawCenteredText(UI_12_FONT_ID, y + static_cast<int>(i) * renderer.getLineHeight(UI_12_FONT_ID),
                                lines[i].c_str());
    }
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }
  preparePixmaps();
  // SD fonts: one pass to collect the page's characters, then load them.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    auto scope = fcm->createPrewarmScope();
    drawPage();
    drawStatusBar();
    scope.endScanAndPrewarm();
    renderer.clearScreen();
    drawPage();
    drawStatusBar();
    const bool full = forceFullRefresh_;
    forceFullRefresh_ = false;
    if (SETTINGS.textAntiAliasing && !renderer.isDarkMode()) {
      ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh_, full);
      ReaderUtils::renderAntiAliased(renderer, [this]() { drawPage(); });
    } else {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh_, full);
    }
    return;
  }
  drawPage();
  drawStatusBar();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh_, forceFullRefresh_);
  forceFullRefresh_ = false;
}
