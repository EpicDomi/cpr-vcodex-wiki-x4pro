#pragma once

// Article layout: turns an article's block stream into pages for the
// reader's font and settings. One engine lays out the body, the infobox,
// figure captions and table cells.
//
// Pages are laid out one at a time from a Cursor (where a page starts), so
// the reader shows the page it wants straight away and counts the rest
// while idle; a Cursor is small and restarts layout exactly where a page
// begins. Nothing here holds more than one block of the article.

#include <XwaContent.h>

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class WikiSession;

namespace wiki {

enum FontSlot : uint8_t { FONT_BODY = 0, FONT_SMALL = 1, FONT_LARGE = 2, FONT_SLOTS = 3 };

// The reader's fonts at three sizes, and the spacing settings.
struct TextStyle {
  int id[FONT_SLOTS] = {};
  int16_t lineHeight[FONT_SLOTS] = {};  // with the reader's line spacing
  int16_t ascent[FONT_SLOTS] = {};      // baseline below the top of a line
  float em[FONT_SLOTS] = {};            // 1 em in pixels
  int8_t tracking = 0;                  // character spacing
  uint8_t wordSpacing = 100;            // percent of a space
  bool justify = true;
  bool hyphenate = false;
  bool extraParagraphSpacing = true;

  static TextStyle fromSettings(const GfxRenderer& renderer);
};

// One thing to draw, in page coordinates (0,0 = top left of the text area).
struct DrawItem {
  enum Kind : uint8_t { Text, Fill, Frame, Image, Formula };
  Kind kind = Text;
  uint8_t font = FONT_BODY;  // Text: slot; Formula: slot it is sized for
  uint8_t style = 0;         // Text: EpdFontFamily style bits
  uint8_t flags = 0;         // Image: image kind (photo, line art, icon)
  int16_t x = 0, y = 0;      // top left (Text: top of its line box)
  int16_t w = 0, h = 0;
  uint32_t a = 0;  // Text: offset in Page::text; Image: image id; Formula: record position
  uint32_t b = 0;  // Text: length; Formula: record length
};

// Something on the page that responds to a tap.
struct HitArea {
  enum Kind : uint8_t { Link, Table, Picture };
  Kind kind = Link;
  bool anchored = false;  // Link: `place` is an anchor hash, else a section
  int16_t x = 0, y = 0, w = 0, h = 0;
  uint16_t group = 0;   // areas of one link share a group (it may wrap)
  uint32_t target = 0;  // Link: article; Table: table index; Picture: image id
  uint32_t place = 0;
};

// Where a page starts: a block, how many of its lines earlier pages showed,
// and the lists and quotes open around it.
struct Cursor {
  static constexpr int MAX_LEVELS = 8;
  struct Level {
    uint8_t kind = 0;    // 0 quote, 1 bullet, 2 numbered, 3 definition
    uint8_t marker = 0;  // the item's marker is still to be drawn
    uint16_t number = 0;
  };
  uint32_t block = 0;  // stream position of the block
  uint16_t line = 0;
  uint8_t depth = 0;   // open lists and quotes (only MAX_LEVELS are tracked)
  bool end = false;    // past the last block
  bool title = false;  // the article's title still comes first (page 1)
  Level levels[MAX_LEVELS];
};

struct Page {
  std::vector<DrawItem> items;
  std::vector<HitArea> areas;
  std::string text;       // NUL-separated text of the Text items
  Cursor start;           // where it begins
  uint32_t startPos = 0;  // body positions of its first and last block
  uint32_t endPos = 0;
  void clear();
};

// Lines of one block before they are placed on a page.
struct Line {
  int16_t h = 0;
  int16_t base = 0;   // baseline below the line's top
  int16_t gap = 0;    // space above, dropped at the top of a page
  uint16_t item = 0;  // first item in BlockLines::items
  uint16_t items = 0;
  uint16_t area = 0;  // first hit area
  uint16_t areas = 0;
  bool keepWithNext = false;  // a heading: never the last line of a page
};

struct BlockLines {
  std::vector<Line> lines;
  std::vector<DrawItem> items;  // y relative to the line's top
  std::vector<HitArea> areas;
  std::string text;
  void clear();
  // Appends `src`'s lines, shifted right by dx.
  void append(const BlockLines& src, int16_t dx);
  // Appends the items and areas of one of src's lines, moved by (dx, dy),
  // to whatever line is being built here.
  void copyItems(const BlockLines& src, const Line& line, int16_t dx, int16_t dy);
  // Starts a line whose items are added next; returns it for its sizes.
  Line& open(int16_t h);
  // Closes the last line opened: it holds every item and area added since.
  void close();
};

// Several columns side by side: line k holds line k of each column, on a
// shared baseline.
void zipColumns(const BlockLines* columns, const int16_t* xs, int count, BlockLines& out);

class Layout {
 public:
  Layout(GfxRenderer& renderer, WikiSession& session, uint32_t article, const xwa::ArticleHeader& header,
         const TextStyle& style, int16_t width, int16_t height);

  // The cursor of the article's first page.
  Cursor first() const;
  // Lays out the page that starts at `at` and returns where the next one
  // starts (next.end once the article is done). False on damaged data.
  bool page(const Cursor& at, Page& out, Cursor& next);
  // Body position of a cursor (for progress and saved places).
  uint32_t bodyPos(const Cursor& c) const;

  int16_t width() const { return width_; }
  int16_t height() const { return height_; }
  const TextStyle& style() const { return style_; }

  // Lays out an inline run at a width (table cells, captions elsewhere).
  bool flowRun(uint32_t pos, uint32_t len, uint8_t font, uint8_t style, int16_t width, uint8_t align, BlockLines& out);

 private:
  friend class Flow;
  bool layBlock(const Cursor& at, const xwa::BlockItem& b, BlockLines& out);
  bool layInfobox(int16_t x, int16_t w, BlockLines& out);
  bool layGallery(const xwa::BlockItem& b, int16_t x, int16_t w, BlockLines& out);
  bool layTablePlaceholder(uint32_t index, int16_t x, int16_t w, BlockLines& out);
  void layPicture(uint32_t image, int16_t x, int16_t w, int16_t maxH, BlockLines& out) const;
  void layFormula(uint32_t pos, uint32_t len, int16_t x, int16_t w, BlockLines& out);
  bool layPre(uint32_t pos, uint32_t len, int16_t x, int16_t w, BlockLines& out);
  int16_t indentOf(const Cursor& c) const;
  void place(const BlockLines& lines, uint16_t index, int16_t y, const Cursor& c, Page& out, uint32_t& groupBase);
  void applyStructure(Cursor& c, const xwa::BlockItem& b) const;

  GfxRenderer& renderer_;
  WikiSession& session_;
  const xwa::ArticleHeader& header_;
  TextStyle style_;
  int16_t width_;
  int16_t height_;
  xwa::StreamReader reader_;

  // The block laid out last, kept so the page after it continues without
  // laying it out again.
  BlockLines cache_;
  uint32_t cacheBlock_ = UINT32_MAX;
  int16_t cacheIndent_ = -1;
};

// The plain text of an inline run (for summaries and labels).
std::string runText(xwa::StreamReader& r, uint32_t pos, uint32_t len, size_t maxBytes = 200);

}  // namespace wiki
