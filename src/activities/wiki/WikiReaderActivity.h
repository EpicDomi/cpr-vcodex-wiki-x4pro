#pragma once

#include <XwaContent.h>

#include <memory>
#include <vector>

#include "WikiLayout.h"
#include "WikiPicture.h"
#include "WikiSearch.h"
#include "WikiSession.h"
#include "activities/Activity.h"

// Reads articles: pages in the reader's font and settings, links followed
// with a way back, contents, tables and pictures opened full screen.
class WikiReaderActivity final : public Activity {
 public:
  // `place.pos` is a body position; with `anchor` set it is an anchor hash
  // (or, with `section`, a section index) to find once the article is open.
  struct Target {
    WikiPlace place;
    bool byAnchor = false;
    bool bySection = false;
  };

  WikiReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<WikiSession> session,
                     Target start, std::vector<WikiPlace> trail = {});
  ~WikiReaderActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void prepareForSleep() override { remember(); }
  bool isReaderActivity() const override { return true; }

 private:
  enum Action : int64_t {
    ACT_CONTENTS = 1,
    ACT_LINKS,
    ACT_SEARCH,
    ACT_BACK,
    ACT_RECENT,
    ACT_RANDOM,
    ACT_HOME,
  };

  bool open(const Target& target);
  void showPage(int index);
  bool turn(int delta);
  bool paginateOne();
  WikiPlace here() const;
  void follow(uint32_t article, uint32_t place, bool anchored);
  void goBack();
  void remember();
  void computeViewport();

  void openMenu();
  void openContents();
  void openLinks();
  void openSearch();
  void openRecent();
  void openTable(uint32_t index);
  void openPicture(uint32_t image);

  void preparePixmaps();
  void drawPage() const;
  void drawStatusBar() const;
  bool handleTap(int x, int y);

  std::shared_ptr<WikiSession> session_;
  Target start_;
  std::vector<WikiPlace> trail_;

  uint32_t article_ = xwa::NO_ARTICLE;
  xwa::ArticleHeader header_;
  std::unique_ptr<wiki::Layout> layout_;
  wiki::TextStyle style_;
  std::vector<wiki::Cursor> pages_;  // where each page laid out so far starts
  bool counted_ = false;             // every page is in pages_
  int pageIndex_ = 0;
  wiki::Page page_;
  std::vector<wiki::Pixmap> pixmaps_;  // per page item: pictures, formulas, fallback-font text
#if defined(BOARD_HAS_PSRAM) || !defined(ESP_PLATFORM)
  static constexpr size_t GLYPH_PIXMAP_BUDGET = 4u << 20;
#else
  static constexpr size_t GLYPH_PIXMAP_BUDGET = 24u << 10;
#endif
  bool pixmapsReady_ = false;
  bool failed_ = false;

  int marginTop_ = 0;
  int marginLeft_ = 0;
  int viewW_ = 0;
  int viewH_ = 0;
  int pagesUntilFullRefresh_ = 0;
  bool forceFullRefresh_ = false;

  std::unique_ptr<WikiSearch> search_;
  std::vector<xwa::Hit> listHits_;
};
