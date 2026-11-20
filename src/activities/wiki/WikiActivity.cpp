#include "WikiActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>

#include "WikiReaderActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/TimeUtils.h"

namespace fui = freeink::ui;

WikiActivity::WikiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string query,
                           std::string archive)
    : UiListActivity("Wiki", renderer, mappedInput), query_(std::move(query)), requested_(std::move(archive)) {}

void WikiActivity::onEnter() {
  UiListActivity::onEnter();
  archives_ = wiki::listArchives();
  std::string path = requested_;
  if (path.empty()) path = wiki::lastArchive();
  if (path.empty() && !archives_.empty()) path = archives_.front();
  openArchive(path);
  if (!query_.empty() && session_) {
    const std::string q = query_;
    query_.clear();
    openSearch(q, true);
  }
}

void WikiActivity::onExit() {
  search_.reset();
  session_.reset();
  UiListActivity::onExit();
}

void WikiActivity::openArchive(const std::string& path) {
  search_.reset();
  session_.reset();
  message_.clear();
  title_ = tr(STR_WIKI);
  if (path.empty()) {
    message_ = tr(STR_WIKI_NO_ARCHIVE);
  } else {
    xwa::Status status = xwa::Status::Ok;
    {
      RenderLock lock(*this);
      session_ = WikiSession::open(path, status);
    }
    if (session_) {
      title_ = session_->title();
    } else {
      message_ = wiki::statusMessage(status);
      title_ = wiki::baseName(path);
    }
  }
  rebuild();
}

uint32_t WikiActivity::todaysArticle() const {
  if (!session_ || !session_->archive().hasDays()) return xwa::NO_ARTICLE;
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now == 0) return xwa::NO_ARTICLE;
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!TimeUtils::getDateFromDayOrdinal(TimeUtils::getLocalDayOrdinal(now), year, month, day)) return xwa::NO_ARTICLE;
  // DAYS counts through a leap year: February 29 is day 59.
  static const uint16_t before[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
  if (month < 1 || month > 12 || day < 1) return xwa::NO_ARTICLE;
  uint32_t id = xwa::NO_ARTICLE;
  if (!session_->archive().dayArticle(before[month - 1] + static_cast<int>(day) - 1, id)) return xwa::NO_ARTICLE;
  return id;
}

void WikiActivity::rebuild() {
  rows_.clear();
  auto add = [this](const char* label, int64_t id, std::string subtitle = {}) {
    WikiRow r;
    r.label = label;
    r.subtitle = std::move(subtitle);
    r.id = id;
    rows_.push_back(std::move(r));
  };
  if (!session_) {
    WikiRow r;
    r.label = message_;
    rows_.push_back(r);
  } else {
    xwa::Archive& ar = session_->archive();
    add(tr(STR_WIKI_SEARCH), ACT_SEARCH);
    const WikiMemory& m = session_->memory();
    std::string title;
    if (m.resume.valid() && ar.title(m.resume.article, title)) add(tr(STR_WIKI_CONTINUE), ACT_CONTINUE, title);
    if (!m.recent.empty()) add(tr(STR_WIKI_RECENT), ACT_RECENT);
    const uint32_t today = todaysArticle();
    if (today != xwa::NO_ARTICLE && ar.title(today, title)) add(tr(STR_WIKI_ON_THIS_DAY), ACT_TODAY, title);
    if (session_->mainArticle() != xwa::NO_ARTICLE && ar.title(session_->mainArticle(), title)) {
      add(tr(STR_WIKI_MAIN_PAGE), ACT_MAIN, title);
    }
    add(tr(STR_WIKI_RANDOM), ACT_RANDOM);
    add(tr(STR_WIKI_INFO), ACT_INFO);
  }
  if (archives_.size() > 1 || (!session_ && !archives_.empty())) {
    add(tr(STR_WIKI_CHANGE), ACT_CHANGE, session_ ? wiki::baseName(session_->path()) : std::string());
  }
  items_.resize(rows_.size());
  for (size_t i = 0; i < rows_.size(); i++) {
    fui::ListItem& it = items_[i];
    it = fui::ListItem{};
    it.label = rows_[i].label.c_str();
    it.subtitle = rows_[i].subtitle.empty() ? nullptr : rows_[i].subtitle.c_str();
    it.actionValue = static_cast<int16_t>(i);
  }
  nav.reset(0);
  requestUpdate();
}

void WikiActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText.maxLines = 4;
  syncListViewport(screen, props);
  screen.list(props);
}

void WikiActivity::activateIndex(int index) {
  if (index < 0 || index >= static_cast<int>(rows_.size()) || rows_[index].id < 0) return;
  app.clearTapFlash();
  switch (rows_[index].id) {
    case ACT_SEARCH:
      openSearch("", false);
      break;
    case ACT_CONTINUE:
      openContinue();
      break;
    case ACT_RECENT:
      openRecent();
      break;
    case ACT_TODAY:
      openArticle(todaysArticle());
      break;
    case ACT_MAIN:
      openArticle(session_->mainArticle());
      break;
    case ACT_RANDOM: {
      const uint32_t n = session_->archive().articleCount();
      if (n > 0) openArticle(static_cast<uint32_t>(random(static_cast<long>(n))));
      break;
    }
    case ACT_INFO:
      openInfo();
      break;
    case ACT_CHANGE:
      chooseArchive();
      break;
    default:
      break;
  }
}

void WikiActivity::openArticle(uint32_t article, uint32_t place, bool anchored) {
  if (!session_ || article == xwa::NO_ARTICLE) return;
  WikiReaderActivity::Target t;
  t.place.article = article;
  t.place.pos = place;
  t.byAnchor = anchored;
  startActivityForResult(std::make_unique<WikiReaderActivity>(renderer, mappedInput, session_, t),
                         [this](const ActivityResult&) { rebuild(); });
}

void WikiActivity::openContinue() {
  const WikiMemory& m = session_->memory();
  WikiReaderActivity::Target t;
  t.place = m.resume;
  startActivityForResult(std::make_unique<WikiReaderActivity>(renderer, mappedInput, session_, t, m.trail),
                         [this](const ActivityResult&) { rebuild(); });
}

void WikiActivity::openSearch(const std::string& text, bool lookup) {
  if (!session_) return;
  if (!search_) search_.reset(new (std::nothrow) WikiSearch(session_));
  if (!search_) return;
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, session_->title(), text, 0);
  keyboard->setSuggestions(search_.get());
  startActivityForResult(std::move(keyboard), [this, lookup](const ActivityResult& result) {
    const auto* kb = std::get_if<KeyboardResult>(&result.data);
    if (result.isCancelled || !kb) {
      // A word looked up from a book goes back to the book.
      if (lookup) {
        ActivityResult r;
        r.isCancelled = true;
        setResult(std::move(r));
        finish();
      }
      return;
    }
    if (kb->suggestion >= 0 && kb->suggestion < search_->count()) {
      const xwa::Hit h = search_->hit(kb->suggestion);
      openArticle(h.article, h.hasAnchor() ? h.anchor : 0, h.hasAnchor());
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
                             openArticle(h.article, h.hasAnchor() ? h.anchor : 0, h.hasAnchor());
                           });
  });
}

void WikiActivity::openRecent() {
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
        if (const auto* pick = std::get_if<IntervalResult>(&result.data)) openArticle(pick->value);
      });
}

void WikiActivity::openInfo() {
  xwa::Archive& ar = session_->archive();
  std::vector<WikiRow> rows;
  auto add = [&rows](const char* label, const std::string& value) {
    if (value.empty()) return;
    WikiRow r;
    r.label = label;
    r.subtitle = value;
    rows.push_back(std::move(r));
  };
  add(tr(STR_WIKI_INFO_TITLE), ar.metaValue("title"));
  add(tr(STR_WIKI_INFO_DESCRIPTION), ar.metaValue("description"));
  add(tr(STR_WIKI_INFO_ARTICLES), ar.metaValue("counts.articles"));
  add(tr(STR_WIKI_INFO_LANGUAGE), ar.metaValue("language"));
  std::string source = ar.metaValue("source.name");
  if (!ar.metaValue("source.date").empty()) source += " (" + ar.metaValue("source.date") + ")";
  add(tr(STR_WIKI_INFO_SOURCE), source);
  add(tr(STR_WIKI_INFO_LICENSE), ar.metaValue("license"));
  add(tr(STR_WIKI_INFO_ATTRIBUTION), ar.metaValue("attribution"));
  char size[32];
  snprintf(size, sizeof(size), "%.1f MB", ar.fileSize() / 1048576.0);
  add(tr(STR_WIKI_INFO_FILE), wiki::baseName(session_->path()) + ".xwa, " + size);
  add(tr(STR_WIKI_INFO_READS), session_->fastReads() ? tr(STR_WIKI_READS_DIRECT) : tr(STR_WIKI_READS_FILE));
  add(tr(STR_WIKI_INFO_CONVERTER), ar.metaValue("converter"));
  startActivityForResult(std::make_unique<WikiListActivity>(renderer, mappedInput, tr(STR_WIKI_INFO), std::move(rows)),
                         [](const ActivityResult&) {});
}

void WikiActivity::chooseArchive() {
  archives_ = wiki::listArchives();
  std::vector<WikiRow> rows;
  int current = 0;
  for (size_t i = 0; i < archives_.size(); i++) {
    WikiRow r;
    r.label = wiki::baseName(archives_[i]);
    r.id = static_cast<int64_t>(i);
    if (session_ && session_->path() == archives_[i]) current = static_cast<int>(i);
    rows.push_back(std::move(r));
  }
  startActivityForResult(
      std::make_unique<WikiListActivity>(renderer, mappedInput, tr(STR_WIKI_ARCHIVES), std::move(rows), current),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* pick = std::get_if<IntervalResult>(&result.data);
        if (pick && pick->value < archives_.size()) openArchive(archives_[pick->value]);
      });
}
