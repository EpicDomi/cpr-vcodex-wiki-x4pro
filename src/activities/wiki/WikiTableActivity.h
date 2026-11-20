#pragma once

#include <XwaContent.h>

#include <map>
#include <memory>
#include <vector>

#include "WikiLayout.h"
#include "WikiPicture.h"
#include "WikiSession.h"
#include "activities/Activity.h"

// A table full screen: cells laid out in the reader's smaller font, columns
// as wide as their text needs (within limits), the screen moving over the
// table a screenful at a time. Links in cells return WikiLinkResult.
class WikiTableActivity final : public Activity {
 public:
  WikiTableActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<WikiSession> session,
                    uint32_t article, uint32_t table);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Cell {
    int16_t x = 0, y = 0, w = 0, h = 0;  // in table coordinates
    bool header = false;
    bool frame = true;  // false for the caption above the table
    uint32_t firstItem = 0, items = 0;
    uint32_t firstArea = 0, areas = 0;
  };

  bool build();
  void scroll(int dx, int dy);
  void draw();

  std::shared_ptr<WikiSession> session_;
  uint32_t article_;
  uint32_t table_;
  xwa::ArticleHeader header_;
  wiki::TextStyle style_;
  std::unique_ptr<wiki::Layout> layout_;
  std::vector<Cell> cells_;
  // All cells' items and link areas, positioned in table coordinates.
  std::vector<wiki::DrawItem> items_;
  std::vector<wiki::HitArea> areas_;
  std::string text_;
  std::map<uint32_t, wiki::Pixmap> pixmaps_;  // by item index, decoded when first seen
  std::vector<int16_t> rowTops_;              // row boundaries, for scrolling by whole rows
  std::vector<int16_t> colLefts_;
  int tableW_ = 0;
  int tableH_ = 0;
  int scrollX_ = 0;
  int scrollY_ = 0;
  int viewX_ = 0, viewY_ = 0, viewW_ = 0, viewH_ = 0;
  bool failed_ = false;
  bool truncated_ = false;
};
