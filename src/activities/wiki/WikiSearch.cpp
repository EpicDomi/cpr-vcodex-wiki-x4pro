#include "WikiSearch.h"

#include <I18n.h>

namespace {
constexpr size_t LIVE_ROWS = 8;
constexpr size_t BEST_ROWS = 12;
constexpr size_t PAGE_ROWS = 30;
}  // namespace

std::string WikiSearch::detailFor(const xwa::Hit& h) const {
  // A redirect shows where it leads.
  std::string target;
  if (h.isRedirect() && session_->archive().title(h.article, target) && target != h.display) {
    return "\xC2\xBB " + target;  // », which the interface fonts have
  }
  return {};
}

void WikiSearch::update(const std::string& text) {
  hits_.clear();
  details_.clear();
  xwa::TitleIndex& index = session_->index();
  std::string key;
  if (!index.fold(text.c_str(), key)) return;
  if (key.empty()) {
    for (const uint32_t a : session_->memory().recent) {
      if (hits_.size() >= LIVE_ROWS) break;
      xwa::Hit h;
      h.article = a;
      if (!session_->archive().title(a, h.display)) continue;
      hits_.push_back(std::move(h));
      details_.emplace_back(tr(STR_WIKI_RECENT_MARK));
    }
    return;
  }
  index.popular(key, LIVE_ROWS, hits_);
  for (const auto& h : hits_) details_.push_back(detailFor(h));
}

void WikiSearch::listAll(const std::string& text, std::vector<WikiRow>& rows, std::vector<xwa::Hit>& hits) {
  rows.clear();
  hits.clear();
  xwa::TitleIndex& index = session_->index();
  key_.clear();
  cursor_ = xwa::IndexCursor();
  if (!index.fold(text.c_str(), key_)) return;
  std::vector<xwa::Hit> best;
  index.popular(key_, BEST_ROWS, best);
  if (!best.empty()) {
    WikiRow h;
    h.label = tr(STR_WIKI_BEST_MATCHES);
    h.header = true;
    rows.push_back(h);
    hits.emplace_back();
    for (const auto& b : best) {
      WikiRow r;
      r.label = b.display;
      r.subtitle = detailFor(b);
      r.id = static_cast<int64_t>(hits.size());
      rows.push_back(std::move(r));
      hits.push_back(b);
    }
  }
  WikiRow h;
  h.label = tr(STR_WIKI_ALL_TITLES);
  h.header = true;
  rows.push_back(h);
  hits.emplace_back();
  listMore(rows, hits);
}

bool WikiSearch::listMore(std::vector<WikiRow>& rows, std::vector<xwa::Hit>& hits) {
  if (cursor_.done) return false;
  std::vector<xwa::Hit> page;
  if (!session_->index().list(key_, cursor_, PAGE_ROWS, page)) return false;
  for (const auto& b : page) {
    WikiRow r;
    r.label = b.display;
    r.subtitle = detailFor(b);
    r.id = static_cast<int64_t>(hits.size());
    rows.push_back(std::move(r));
    hits.push_back(b);
  }
  return !page.empty() && !cursor_.done;
}
