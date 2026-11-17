#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

struct WikiRow {
  std::string label;
  std::string subtitle;
  std::string value;
  int64_t id = -1;      // what the caller gets back; rows below 0 do nothing
  bool header = false;  // a section heading
};

// A list of wiki things (contents, recent articles, results, menus, the
// archive's details). Choosing a row returns IntervalResult{its id}.
// `more` is asked for further rows when the selection nears the end (long
// title listings are read from the index as the reader scrolls).
class WikiListActivity final : public UiListActivity {
 public:
  using MoreFn = std::function<bool(std::vector<WikiRow>& rows)>;

  WikiListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, std::vector<WikiRow> rows,
                   int selected = 0, MoreFn more = {});

  void onEnter() override;

 protected:
  int listCount() const override { return static_cast<int>(items_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }
  void onBackButton() override;
  bool handleCustomInput() override;

 private:
  void rebuild();
  void fetchMore();

  std::string title_;
  std::vector<WikiRow> rows_;
  std::vector<freeink::ui::ListItem> items_;
  int initial_ = 0;
  MoreFn more_;
  bool exhausted_ = false;
};
