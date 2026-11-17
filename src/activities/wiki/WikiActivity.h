#pragma once

#include <memory>
#include <string>
#include <vector>

#include "WikiListActivity.h"
#include "WikiSearch.h"
#include "WikiSession.h"
#include "activities/UiListActivity.h"

// The wiki's first screen: search, carry on reading, recent and random
// articles, the day's article, the archive's main page and details, and the
// choice of archive when the card holds several.
class WikiActivity final : public UiListActivity {
 public:
  // `query`: open straight into search with this text (a word looked up
  // from a book), and leave the wiki when that search is closed.
  // `archive`: open this .xwa rather than the last one used.
  explicit WikiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string query = "",
                        std::string archive = "");

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override { return static_cast<int>(items_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }

 private:
  enum Action : int64_t {
    ACT_SEARCH = 1,
    ACT_CONTINUE,
    ACT_RECENT,
    ACT_TODAY,
    ACT_MAIN,
    ACT_RANDOM,
    ACT_INFO,
    ACT_CHANGE,
  };

  void openArchive(const std::string& path);
  void rebuild();
  void openSearch(const std::string& text, bool lookup);
  void openArticle(uint32_t article, uint32_t place = 0, bool anchored = false);
  void openContinue();
  void openRecent();
  void openInfo();
  void chooseArchive();
  uint32_t todaysArticle() const;

  std::string query_;
  std::string requested_;
  std::string title_;
  std::string message_;
  std::shared_ptr<WikiSession> session_;
  std::unique_ptr<WikiSearch> search_;
  std::vector<xwa::Hit> listHits_;
  std::vector<WikiRow> rows_;
  std::vector<freeink::ui::ListItem> items_;
  std::vector<std::string> archives_;
};
