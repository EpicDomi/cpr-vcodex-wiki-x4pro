#pragma once

// Article content (spec 7.2-7.4, 8.1): a reader over an article's byte
// stream, which may cross chunk boundaries, its header, and pull parsers
// for the block stream, inline runs, tables, the infobox and formulas.
// Nothing here holds more than the chunk the archive caches.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "XwaArchive.h"

namespace xwa {

// Byte reader over one article stream. Positions count from the stream's
// first byte (its stream_size field), so position p is in chunk
// first_chunk + ((offset_in_chunk + p) >> chunk_bits).
class StreamReader {
 public:
  bool open(Archive& archive, uint32_t articleId);
  uint32_t pos() const { return pos_; }
  uint32_t size() const { return size_; }  // the whole stream, size field included
  bool seek(uint32_t pos);
  bool u8(uint8_t& v);
  bool u16(uint16_t& v);
  bool u32(uint32_t& v);
  bool varint(uint64_t& v);
  bool varint32(uint32_t& v);
  bool svarint32(int32_t& v);
  bool bytes(void* dst, size_t len);
  bool skip(uint32_t len);
  bool string(std::string& out, uint32_t maxLen = 65535);
  Archive* archive() const { return ar_; }

 private:
  bool load();

  Archive* ar_ = nullptr;
  uint32_t firstChunk_ = 0;
  uint32_t offset0_ = 0;
  uint32_t pos_ = 0;
  uint32_t size_ = 0;
  // The loaded chunk, valid while the archive's chunk epoch is unchanged.
  const uint8_t* data_ = nullptr;
  uint32_t dataChunk_ = UINT32_MAX;
  uint32_t dataLen_ = 0;
  uint32_t epoch_ = 0;
};

struct TocEntry {
  uint8_t level = 0;
  std::string text;
  uint32_t pos = 0;  // from the start of the body
};

struct AnchorEntry {
  uint32_t hash = 0;
  uint32_t section = 0;
};

struct ArticleHeader {
  std::string title;
  uint32_t bodyStart = 0;  // stream position of the body's first byte
  uint32_t bodySize = 0;
  std::vector<TocEntry> toc;
  std::vector<uint32_t> tables;  // body-relative positions
  bool hasInfobox = false;
  uint32_t infobox = 0;
  std::vector<AnchorEntry> anchors;

  uint32_t bodyEnd() const { return bodyStart + bodySize; }
  uint32_t at(uint32_t bodyPos) const { return bodyStart + bodyPos; }
};

// Reads the header at the start of the stream.
bool readHeader(StreamReader& r, ArticleHeader& out);

// Anchor hash (7.2): FNV-1a over UTF-8 with spaces taken as '_'.
uint32_t anchorHash(const char* name, size_t len);
// The anchor a contents entry gets by rule: trimmed, inner space runs as '_'.
uint32_t headingAnchorHash(const std::string& text);
// The section (1-based contents entry, 0 = top) a named place is in, or -1.
int findPlace(const ArticleHeader& h, uint32_t hash);

// One block of the block stream (7.3), read at the reader's position.
struct BlockItem {
  uint8_t op = 0;      // block_op, or 0 at the end of the body
  uint8_t arg = 0;     // heading level, list kind
  uint32_t start = 0;  // stream position of the opcode
  // PARA, HEADING, FIGURE (caption): the inline run's ops.
  uint32_t runPos = 0;
  uint32_t runLen = 0;
  uint32_t value = 0;  // FIGURE image, TABLE_REF index, GALLERY item count
  // PRE text, MATH_BLOCK formula record (after its length field), GALLERY items.
  uint32_t dataPos = 0;
  uint32_t dataLen = 0;
  uint32_t next = 0;  // stream position of the following block
};
// False on damaged data. At `end` returns true with op 0.
bool readBlock(StreamReader& r, uint32_t end, BlockItem& out);

// One inline op (7.3).
struct InlineItem {
  enum Kind : uint8_t { Text, StylePush, StylePop, Link, LinkEnd, Math, Icon, Break, NoteRef };
  Kind kind = Text;
  uint8_t style = 0;      // StylePush
  uint8_t height = 0;     // Icon: sixteenths of the body text size
  bool anchored = false;  // Link: `place` is an anchor hash, else a section
  uint32_t article = 0;
  uint32_t place = 0;
  uint32_t pos = 0;  // Text: bytes at pos; Math: the record after its length
  uint32_t len = 0;
  uint32_t value = 0;  // Icon image, NoteRef number
};
// Reads the next op before `runEnd`; skips EXT ops. False at the end
// (check `damaged`).
bool readInline(StreamReader& r, uint32_t runEnd, InlineItem& out, bool& damaged);

// A GALLERY's items, read one by one from BlockItem::dataPos.
bool readGalleryItem(StreamReader& r, uint32_t& image, uint32_t& runPos, uint32_t& runLen);

// Tables and the infobox (7.4).
struct TableCell {
  uint16_t row = 0;
  uint16_t col = 0;
  uint16_t rowspan = 1;
  uint16_t colspan = 1;
  bool header = false;
  uint32_t runPos = 0;
  uint32_t runLen = 0;
};
struct TableInfo {
  uint32_t rows = 0;
  uint32_t cols = 0;
  uint8_t flags = 0;
  std::vector<TableCell> cells;  // row and col worked out as a browser would
  uint32_t captionPos = 0;       // the caption's inline run (empty when none)
  uint32_t captionLen = 0;
};
bool readTable(StreamReader& r, const ArticleHeader& h, uint32_t index, TableInfo& out, uint32_t maxCells = 4096);

struct InfoboxRow {
  uint8_t kind = 0;  // InfoboxRowKind
  uint32_t image = 0;
  uint32_t runPos = 0;  // header, image caption, field label, full
  uint32_t runLen = 0;
  uint32_t run2Pos = 0;  // field value
  uint32_t run2Len = 0;
};
bool readInfobox(StreamReader& r, const ArticleHeader& h, std::vector<InfoboxRow>& out);

// A formula record (8.1) held in memory.
struct Formula {
  bool display = false;
  bool hasFallback = false;
  uint32_t fallbackImage = 0;
  int32_t width = 0;
  int32_t ascent = 0;
  int32_t descent = 0;
  const uint8_t* items = nullptr;
  const uint8_t* end = nullptr;
};
// Loads the formula record at [pos, pos + len) (a MATH op's or MATH_BLOCK's
// data, after its length field) into `buf` and parses its head.
bool loadFormula(StreamReader& r, uint32_t pos, uint32_t len, std::vector<uint8_t>& buf, Formula& out);
bool parseFormula(const uint8_t* data, size_t len, Formula& out);

struct MathItem {
  enum Kind : uint8_t { Glyph, GlyphXY, Rule, Path };
  Kind kind = Glyph;
  uint32_t glyph = 0;
  uint8_t scale = 0;  // Glyph: MATH_GLYPHS scale index
  int16_t sx = 256;   // GlyphXY, 8.8
  int16_t sy = 256;
  int32_t x = 0;  // absolute position from the formula's origin (font units, y up)
  int32_t y = 0;
  uint32_t w = 0;  // Rule
  uint32_t h = 0;
  const uint8_t* path = nullptr;  // Path: command_count then commands
  const uint8_t* pathEnd = nullptr;
};
// Iterates a formula's items; `x`/`y` accumulate. False at the end or on an
// item it does not know (then `unknown` is set).
class MathItems {
 public:
  explicit MathItems(const Formula& f) : p_(f.items), end_(f.end) {}
  bool next(MathItem& out);
  bool unknown() const { return unknown_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  int32_t x_ = 0;
  int32_t y_ = 0;
  bool unknown_ = false;
};

// Path commands of a PATH item, with absolute coordinates relative to the
// item's position.
struct PathCommand {
  uint8_t op = 0;  // 0 move, 1 line, 2 quad, 3 cubic, 4 close
  int32_t pt[6] = {};
};
class PathCommands {
 public:
  explicit PathCommands(const MathItem& item);
  bool next(PathCommand& out);
  bool bad() const { return bad_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  uint32_t remaining_ = 0;
  int32_t penX_ = 0;
  int32_t penY_ = 0;
  bool bad_ = false;
};

}  // namespace xwa
