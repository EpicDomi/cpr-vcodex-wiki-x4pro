#pragma once

#include <XwaSearch.h>

#include <memory>
#include <string>
#include <vector>

#include "WikiListActivity.h"
#include "WikiSession.h"
#include "activities/util/KeyboardEntryActivity.h"

// Live search results for the keyboard: the most read articles whose title
// starts with what was typed, or the recently read ones while nothing is.
class WikiSearch final : public KeyboardSuggestions {
 public:
  explicit WikiSearch(std::shared_ptr<WikiSession> session) : session_(std::move(session)) {}

  void update(const std::string& text) override;
  int count() const override { return static_cast<int>(hits_.size()); }
  const char* label(int index) const override { return hits_[index].display.c_str(); }
  const char* detail(int index) const override { return details_[index].c_str(); }
  const xwa::Hit& hit(int index) const { return hits_[index]; }

  // Rows for the full results of `text`: the best matches, then every title
  // in order, read from the index as the list scrolls.
  void listAll(const std::string& text, std::vector<WikiRow>& rows, std::vector<xwa::Hit>& hits);
  bool listMore(std::vector<WikiRow>& rows, std::vector<xwa::Hit>& hits);

 private:
  std::string detailFor(const xwa::Hit& h) const;

  std::shared_ptr<WikiSession> session_;
  std::vector<xwa::Hit> hits_;
  std::vector<std::string> details_;
  std::string key_;
  xwa::IndexCursor cursor_;
};
