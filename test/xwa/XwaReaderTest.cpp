// The wiki reader core (lib/Xwa) against archives written by xwa-tool.
//
// test/xwa/gen writes two archives (full and compact profile) with every
// block, inline, section and search case the firmware reads, plus a dump of
// how xwa-tool's own reader reads them. DumpMatches reads the same archives
// through lib/Xwa and prints the same dump: any difference is a reader bug.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "XwaArchive.h"
#include "XwaContent.h"
#include "XwaCrc.h"
#include "XwaFont.h"
#include "XwaLa2.h"
#include "XwaMath.h"
#include "XwaSearch.h"

using namespace xwa;

namespace {

std::vector<uint8_t> loadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class MemorySource : public Source {
 public:
  explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  uint64_t size() override { return bytes_.size(); }
  bool read(uint64_t offset, void* buf, size_t len) override {
    reads++;
    if (offset > bytes_.size() || len > bytes_.size() - offset) return false;
    std::memcpy(buf, bytes_.data() + offset, len);
    return true;
  }
  std::vector<uint8_t>& bytes() { return bytes_; }
  int reads = 0;

 private:
  std::vector<uint8_t> bytes_;
};

std::string dataPath(const char* name) { return std::string(XWA_TEST_DATA) + "/" + name; }

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

std::string esc(const std::string& s) {
  std::string o = "\"";
  for (const char c : s) {
    if (c == '\\') {
      o += "\\\\";
    } else if (c == '"') {
      o += "\\\"";
    } else if (c == '\n') {
      o += "\\n";
    } else {
      o += c;
    }
  }
  return o + "\"";
}

// ------------------------------------------------------------------ dump

class Dumper {
 public:
  explicit Dumper(Archive& ar) : ar_(ar) {}

  std::string formula(const Formula& f) {
    std::string o = fmt("disp=%d w=%d asc=%d desc=%d fb=", f.display ? 1 : 0, f.width, f.ascent, f.descent);
    o += f.hasFallback ? std::to_string(f.fallbackImage) : "-";
    MathItems items(f);
    MathItem it;
    int32_t px = 0;
    int32_t py = 0;
    while (items.next(it)) {
      const int32_t dx = it.x - px;
      const int32_t dy = it.y - py;
      px = it.x;
      py = it.y;
      switch (it.kind) {
        case MathItem::Glyph:
          o += fmt(" G%us%u(%d,%d)", it.glyph, it.scale, dx, dy);
          break;
        case MathItem::GlyphXY:
          o += fmt(" X%u[%d,%d](%d,%d)", it.glyph, it.sx, it.sy, dx, dy);
          break;
        case MathItem::Rule:
          o += fmt(" R%ux%u(%d,%d)", it.w, it.h, dx, dy);
          break;
        case MathItem::Path: {
          o += fmt(" P(%d,%d)", dx, dy);
          PathCommands cmds(it);
          PathCommand c;
          while (cmds.next(c)) {
            switch (c.op) {
              case 0:
                o += fmt("M%d,%d", c.pt[0], c.pt[1]);
                break;
              case 1:
                o += fmt("L%d,%d", c.pt[0], c.pt[1]);
                break;
              case 2:
                o += fmt("Q%d,%d,%d,%d", c.pt[0], c.pt[1], c.pt[2], c.pt[3]);
                break;
              case 3:
                o += fmt("C%d,%d,%d,%d,%d,%d", c.pt[0], c.pt[1], c.pt[2], c.pt[3], c.pt[4], c.pt[5]);
                break;
              default:
                o += "Z";
            }
          }
          EXPECT_FALSE(cmds.bad());
          break;
        }
      }
    }
    EXPECT_FALSE(items.unknown());
    return o;
  }

  std::string text(StreamReader& r, uint32_t pos, uint32_t len) {
    std::string t(len, '\0');
    EXPECT_TRUE(r.seek(pos) && (len == 0 || r.bytes(&t[0], len)));
    return t;
  }

  std::string run(StreamReader& r, uint32_t pos, uint32_t len) {
    std::string o;
    EXPECT_TRUE(r.seek(pos));
    InlineItem it;
    bool damaged = false;
    std::vector<uint8_t> buf;
    while (readInline(r, pos + len, it, damaged)) {
      const uint32_t back = r.pos();
      switch (it.kind) {
        case InlineItem::Text:
          o += esc(text(r, it.pos, it.len));
          break;
        case InlineItem::StylePush:
          o += fmt("<s%u>", it.style);
          break;
        case InlineItem::StylePop:
          o += "</s>";
          break;
        case InlineItem::Link:
          o += it.anchored ? fmt("<a%u@%08x>", it.article, it.place) : fmt("<a%u#%u>", it.article, it.place);
          break;
        case InlineItem::LinkEnd:
          o += "</a>";
          break;
        case InlineItem::Math: {
          Formula f;
          EXPECT_TRUE(loadFormula(r, it.pos, it.len, buf, f));
          o += "<m " + formula(f) + ">";
          break;
        }
        case InlineItem::Icon:
          o += fmt("<i%uh%u>", it.value, it.height);
          break;
        case InlineItem::Break:
          o += "<br>";
          break;
        case InlineItem::NoteRef:
          o += fmt("<n%u>", it.value);
          break;
      }
      r.seek(back);
    }
    EXPECT_FALSE(damaged);
    return o;
  }

  void blocks(std::string& o, StreamReader& r, const ArticleHeader& h) {
    ASSERT_TRUE(r.seek(h.bodyStart));
    int depth = 1;
    std::vector<int> open;
    BlockItem b;
    std::vector<uint8_t> buf;
    for (;;) {
      ASSERT_TRUE(readBlock(r, h.bodyEnd(), b)) << "block at " << r.pos();
      if (b.op == 0) break;
      const std::string pad(2 * depth, ' ');
      switch (b.op) {
        case block_op::PARA:
          o += pad + "PARA " + run(r, b.runPos, b.runLen) + "\n";
          break;
        case block_op::HEADING:
          o += pad + fmt("H%u ", b.arg) + run(r, b.runPos, b.runLen) + "\n";
          break;
        case block_op::LIST_START:
          o += pad + fmt("LIST %u\n", b.arg);
          open.push_back(depth);
          depth += 2;
          break;
        case block_op::LIST_ITEM:
          ASSERT_FALSE(open.empty());
          o += std::string(2 * (open.back() + 1), ' ') + "ITEM\n";
          break;
        case block_op::LIST_END:
        case block_op::QUOTE_END:
          ASSERT_FALSE(open.empty());
          depth = open.back();
          open.pop_back();
          o += std::string(2 * depth, ' ') + (b.op == block_op::LIST_END ? "ENDLIST\n" : "ENDQUOTE\n");
          break;
        case block_op::QUOTE_START:
          o += pad + "QUOTE\n";
          open.push_back(depth);
          depth += 1;
          break;
        case block_op::PRE:
          o += pad + "PRE " + esc(text(r, b.dataPos, b.dataLen)) + "\n";
          break;
        case block_op::RULE:
          o += pad + "RULE\n";
          break;
        case block_op::FIGURE:
          o += pad + fmt("FIG %u ", b.value) + run(r, b.runPos, b.runLen) + "\n";
          break;
        case block_op::MATH_BLOCK: {
          Formula f;
          EXPECT_TRUE(loadFormula(r, b.dataPos, b.dataLen, buf, f));
          o += pad + "MATH " + formula(f) + "\n";
          break;
        }
        case block_op::TABLE_REF:
          o += pad + fmt("TABLEREF %u\n", b.value);
          break;
        case block_op::INFOBOX_REF:
          o += pad + "INFOBOXREF\n";
          break;
        case block_op::GALLERY: {
          o += pad + fmt("GALLERY %u\n", b.value);
          uint32_t at = b.dataPos;
          for (uint32_t i = 0; i < b.value; i++) {
            uint32_t image = 0, rp = 0, rl = 0;
            ASSERT_TRUE(r.seek(at) && readGalleryItem(r, image, rp, rl));
            at = r.pos();
            o += pad + fmt("  GITEM %u ", image) + run(r, rp, rl) + "\n";
          }
          break;
        }
        default:
          ADD_FAILURE() << "block op " << int(b.op);
      }
      ASSERT_TRUE(r.seek(b.next));
    }
    EXPECT_TRUE(open.empty());
  }

  void hits(std::string& o, const char* kind, const std::vector<Hit>& list) {
    for (const Hit& h : list) {
      o += fmt("  %s ", kind) + esc(h.display) + fmt(" a=%u f=%u p=%u h=", h.article, h.flags, h.popularity);
      o += h.hasAnchor() ? fmt("%08x", h.anchor) : "-";
      o += "\n";
    }
  }

  std::string dump() {
    std::string o;
    o += fmt("SB profile=%u codec=%u chunk_bits=%u req=%x opt=%x articles=%u chunks=%u images=%u\n", ar_.profile(),
             ar_.textCodec(), ar_.chunkBits(), ar_.requiredFeatures(), ar_.optionalFeatures(), ar_.articleCount(),
             ar_.chunkCount(), ar_.imageCount());
    for (const auto& kv : ar_.meta()) o += "META " + kv.first + "=" + kv.second + "\n";
    for (uint32_t id = 0; id < ar_.articleCount(); id++) {
      ArticleRecord rec;
      EXPECT_TRUE(ar_.articleRecord(id, rec));
      StreamReader r;
      EXPECT_TRUE(r.open(ar_, id)) << "article " << id;
      ArticleHeader h;
      EXPECT_TRUE(readHeader(r, h)) << "article " << id;
      std::string title;
      EXPECT_TRUE(ar_.title(id, title));
      o += fmt("ARTICLE %u ", id) + esc(h.title) +
           fmt(" chunks=%u,%u,%u flags=%u pop=%u title=", rec.firstChunk, rec.offsetInChunk, rec.chunkCount, rec.flags,
               rec.popularity) +
           esc(title) + "\n";
      o += fmt("BODYSIZE %u\n", h.bodySize);
      for (const auto& e : h.toc) o += fmt("TOC %u %u ", e.level, e.pos) + esc(e.text) + "\n";
      o += "TABLES ";
      for (size_t i = 0; i < h.tables.size(); i++) o += (i ? " " : "") + std::to_string(h.tables[i]);
      o += "\n";
      o += h.hasInfobox ? fmt("INFOBOX %u\n", h.infobox) : std::string("INFOBOX -\n");
      for (const auto& a : h.anchors) o += fmt("ANCHOR %08x %u\n", a.hash, a.section);
      o += "BODY\n";
      blocks(o, r, h);
      for (uint32_t i = 0; i < h.tables.size(); i++) {
        TableInfo t;
        EXPECT_TRUE(readTable(r, h, i, t));
        o +=
            fmt("TABLE %u %ux%u f=%u caption=", i, t.rows, t.cols, t.flags) + run(r, t.captionPos, t.captionLen) + "\n";
        for (const auto& c : t.cells) {
          o += fmt("  CELL h=%d %ux%u ", c.header ? 1 : 0, c.rowspan, c.colspan) + run(r, c.runPos, c.runLen) + "\n";
        }
      }
      if (h.hasInfobox) {
        std::vector<InfoboxRow> rows;
        EXPECT_TRUE(readInfobox(r, h, rows));
        o += fmt("INFOBOXROWS %zu\n", rows.size());
        for (const auto& row : rows) {
          switch (row.kind) {
            case 0:
              o += "  IHEAD " + run(r, row.runPos, row.runLen) + "\n";
              break;
            case 1:
              o += fmt("  IIMG %u ", row.image) + run(r, row.runPos, row.runLen) + "\n";
              break;
            case 2:
              o += "  IFIELD " + run(r, row.runPos, row.runLen) + " | " + run(r, row.run2Pos, row.run2Len) + "\n";
              break;
            default:
              o += "  IFULL " + run(r, row.runPos, row.runLen) + "\n";
          }
        }
      }
      o += "END\n";
    }
    const char* queries[] = {"",       "a",         "al",           "alpha",    "ALPHA",  "Alfa",
                             "ça",     "ca",        "strasse",      "STRASSE",  "straße", "STRAẞE",
                             "कि",     "क",         "が",           "か",       "한",     "한국",
                             "كتاب",   "كِتَاب",      "שָׁלוֹם",         "ein",      "eins",   "e",
                             "f",      "filler 01", "Filler_0399",  "zzz",      "beta h", "beta's",
                             "januar", "mercury",   "  einstein  ", "ÉINSTEIN", "ﬁller",  "ｆｉｌｌｅｒ"};
    TitleIndex index(ar_);
    for (const char* q : queries) {
      std::string key;
      EXPECT_TRUE(index.fold(q, key));
      std::string hex;
      for (const unsigned char c : key) hex += fmt("%02x", c);
      o += "QUERY " + esc(q) + " key=" + hex + "\n";
      IndexCursor cursor;
      std::vector<Hit> list;
      EXPECT_TRUE(index.list(key, cursor, 12, list));
      hits(o, "IDX", list);
      std::vector<Hit> pop;
      EXPECT_TRUE(index.popular(key, 8, pop));
      hits(o, "POP", pop);
    }
    for (int d = 0; d < DAYS_IN_YEAR; d++) {
      uint32_t a = 0;
      if (ar_.dayArticle(d, a)) o += fmt("DAY %d %u\n", d, a);
    }
    for (uint32_t id = 0; id < ar_.imageCount(); id++) {
      ImageRecord rec;
      EXPECT_TRUE(ar_.imageRecord(id, rec));
      std::vector<uint8_t> data(rec.size);
      EXPECT_TRUE(ar_.readImage(rec, 0, data.data(), data.size()));
      o += fmt("IMAGE %u enc=%u %ux%u kind=%u flags=%u size=%u crc=%08x\n", id, rec.encoding, rec.width, rec.height,
               rec.kind, rec.flags, rec.size, crc32(0, data.data(), data.size()));
      if (rec.encoding == image_encoding::LINE_ART) {
        La2Reader la2;
        EXPECT_TRUE(la2.begin(ar_, rec));
        std::vector<uint8_t> levels(la2.width());
        uint32_t crc = 0;
        for (uint16_t y = 0; y < la2.height(); y++) {
          EXPECT_TRUE(la2.row(levels.data())) << "image " << id << " row " << y;
          crc = crc32(crc, levels.data(), levels.size());
        }
        EXPECT_FALSE(la2.row(levels.data()));
        o += fmt("LA2 %u bpp=%u codec=%u levels_crc=%08x\n", id, la2.bpp(), la2Codec(ar_, rec), crc);
      }
    }
    GlyphTable glyphs;
    if (glyphs.open(ar_)) {
      o += fmt("GLYPHS upem=%u count=%u scales=", glyphs.upem(), glyphs.count());
      for (size_t i = 0; i < glyphs.scales().size(); i++) o += (i ? "," : "") + std::to_string(glyphs.scales()[i]);
      o += "\n";
      std::vector<uint8_t> g;
      for (uint32_t i = 0; i < glyphs.count(); i++) {
        EXPECT_TRUE(glyphs.glyph(i, g));
        o += fmt("GLYPH %u bbox=%d,%d,%d,%d ops=%d\n", i, static_cast<int16_t>(le16(g.data())),
                 static_cast<int16_t>(le16(g.data() + 2)), static_cast<int16_t>(le16(g.data() + 4)),
                 static_cast<int16_t>(le16(g.data() + 6)), countOps(g));
      }
    }
    const uint32_t fontSections[] = {section::FONT, section::FONT_BOLD, section::FONT_ITALIC,
                                     section::FONT_BOLD_ITALIC};
    for (const uint32_t type : fontSections) {
      FontSection f;
      if (!f.open(ar_, type)) continue;
      o += fmt("FONT %u upem=%u asc=%d desc=%d glyphs=%u\n", type, f.upem(), f.ascender(), f.descender(),
               f.glyphCount());
      std::vector<uint8_t> g;
      for (uint32_t i = 0; i < f.rangeCount(); i++) {
        uint32_t first = 0, last = 0, glyph = 0;
        EXPECT_TRUE(f.range(i, first, last, glyph));
        for (uint32_t cp = first; cp <= last; cp++) {
          EXPECT_EQ(f.find(cp), static_cast<int32_t>(glyph + cp - first));
          EXPECT_TRUE(f.glyph(glyph + cp - first, g));
          o += fmt("FG %04X adv=%u bbox=%d,%d,%d,%d ops=%d\n", cp, f.advance(glyph + cp - first),
                   static_cast<int16_t>(le16(g.data())), static_cast<int16_t>(le16(g.data() + 2)),
                   static_cast<int16_t>(le16(g.data() + 4)), static_cast<int16_t>(le16(g.data() + 6)), countOps(g));
        }
      }
    }
    const std::pair<uint32_t, const char*> places[] = {{1, "History_2"}, {1, "Career"},          {1, "Early_life"},
                                                       {1, "History"},   {1, "Details"},         {0, "Old_name"},
                                                       {0, "Lists"},     {0, "Quotes_and_code"}, {1, "Nope"}};
    for (const auto& p : places) {
      StreamReader r;
      ArticleHeader h;
      EXPECT_TRUE(r.open(ar_, p.first) && readHeader(r, h));
      const uint32_t hash = anchorHash(p.second, std::strlen(p.second));
      const int place = findPlace(h, hash);
      o += fmt("PLACE %u %08x ", p.first, hash) + (place < 0 ? std::string("-") : std::to_string(place)) + "\n";
    }
    return o;
  }

 private:
  static int countOps(const std::vector<uint8_t>& g) {
    static const int args[6] = {4, 4, 8, 12, 0, 0};
    int n = 0;
    for (size_t p = 8; p < g.size();) {
      const uint8_t op = g[p++];
      if (op == 5) return n;
      if (op > 5) return -1;
      p += args[op];
      n++;
    }
    return -1;
  }
  static unsigned la2Codec(Archive& ar, const ImageRecord& rec) {
    uint8_t head[6];
    return ar.readImage(rec, 0, head, 6) ? head[5] : 255;
  }

  Archive& ar_;
};

std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

void expectDump(const char* name, const Archive::Options& opts) {
  MemorySource src(loadFile(dataPath((std::string(name) + ".xwa").c_str())));
  ASSERT_GT(src.size(), 0u);
  Archive ar;
  ASSERT_EQ(ar.open(&src, opts), Status::Ok);
  const std::string actual = Dumper(ar).dump();
  {
    std::ofstream out(std::string(name) + ".actual.txt", std::ios::binary);
    out << actual;
  }
  const auto expected = loadFile(dataPath((std::string(name) + ".expected.txt").c_str()));
  const auto want = lines(std::string(expected.begin(), expected.end()));
  const auto got = lines(actual);
  int shown = 0;
  for (size_t i = 0; i < std::max(want.size(), got.size()) && shown < 12; i++) {
    const std::string w = i < want.size() ? want[i] : "<missing>";
    const std::string g = i < got.size() ? got[i] : "<missing>";
    if (w != g) {
      ADD_FAILURE() << name << " line " << (i + 1) << "\n  want: " << w << "\n  got:  " << g;
      shown++;
    }
  }
  EXPECT_EQ(want.size(), got.size());
}

const Archive::Options kX4{2, 1};
const Archive::Options kPro{128, 4};

}  // namespace

// ------------------------------------------------------------------ tests

TEST(XwaCrc, CheckValue) {
  EXPECT_EQ(crc32(0, "123456789", 9), 0xCBF43926u);
  // Chained calls equal one call.
  EXPECT_EQ(crc32(crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
  EXPECT_EQ(crc32(0, "", 0), 0u);
}

TEST(XwaDump, FullProfileMatchesXwaTool) { expectDump("rich-full", kPro); }
TEST(XwaDump, CompactProfileMatchesXwaTool) { expectDump("rich-compact", kPro); }
// The X4's budget: two cached blocks and one chunk slot, so nearly every
// read misses and evicts. The results must not change.
TEST(XwaDump, FullProfileWithX4Cache) { expectDump("rich-full", kX4); }
TEST(XwaDump, CompactProfileWithX4Cache) { expectDump("rich-compact", kX4); }

namespace {
// Rewrites the superblock CRC after a test edits the superblock.
void resealSuperblock(std::vector<uint8_t>& b) {
  const uint32_t c = crc32(0, b.data(), 4092);
  for (int i = 0; i < 4; i++) b[4092 + i] = static_cast<uint8_t>(c >> (8 * i));
}
Status openBytes(std::vector<uint8_t> bytes) {
  MemorySource src(std::move(bytes));
  Archive ar;
  return ar.open(&src, kX4);
}
}  // namespace

TEST(XwaOpen, RefusesWhatItCannotRead) {
  const auto good = loadFile(dataPath("rich-compact.xwa"));
  ASSERT_EQ(openBytes(good), Status::Ok);

  auto b = good;
  b[0] = 'Y';
  EXPECT_EQ(openBytes(b), Status::NotXwa);

  b = good;
  b.resize(b.size() - 100);  // an interrupted copy
  EXPECT_EQ(openBytes(b), Status::Incomplete);

  b = good;
  b.push_back(0);
  EXPECT_EQ(openBytes(b), Status::Damaged);

  b = good;
  b[100] ^= 1;  // superblock bit flip
  EXPECT_EQ(openBytes(b), Status::Damaged);

  b = good;
  b[10] = 3;  // version 0.3
  resealSuperblock(b);
  EXPECT_EQ(openBytes(b), Status::TooOld);

  b = good;
  b[10] = 5;  // version 0.5
  resealSuperblock(b);
  EXPECT_EQ(openBytes(b), Status::TooNew);

  b = good;
  b[27] |= 0x80;  // an unknown required feature
  resealSuperblock(b);
  EXPECT_EQ(openBytes(b), Status::UnsupportedFeature);

  b = good;
  b[29] |= 0x80;  // unknown optional features are fine
  resealSuperblock(b);
  EXPECT_EQ(openBytes(b), Status::Ok);

  EXPECT_EQ(openBytes(std::vector<uint8_t>(10, 0)), Status::NotXwa);
  EXPECT_EQ(openBytes({}), Status::ReadError);
}

TEST(XwaOpen, DamagedChunkIsRefusedNotMisread) {
  for (const char* name : {"rich-compact.xwa", "rich-full.xwa"}) {
    MemorySource src(loadFile(dataPath(name)));
    Archive ar;
    ASSERT_EQ(ar.open(&src, kX4), Status::Ok);
    ChunkInfo ci;
    ASSERT_TRUE(ar.chunkInfo(1, ci));
    uint32_t len = 0;
    ASSERT_NE(ar.chunk(1, len), nullptr);
    const uint64_t blobs = ar.sectionInfo(section::BLOBS).offset;
    ar.close();
    // Flip one bit in the middle of chunk 1's stored bytes.
    src.bytes()[blobs + ci.offset + ci.storedSize / 2] ^= 0x10;
    Archive ar2;
    ASSERT_EQ(ar2.open(&src, kX4), Status::Ok);
    EXPECT_EQ(ar2.chunk(1, len), nullptr) << name;
    EXPECT_NE(ar2.chunk(0, len), nullptr) << name;
  }
}

TEST(XwaSearch, ContinuedListingMatchesOneListing) {
  MemorySource src(loadFile(dataPath("rich-compact.xwa")));
  Archive ar;
  ASSERT_EQ(ar.open(&src, kX4), Status::Ok);
  TitleIndex index(ar);
  std::string key;
  ASSERT_TRUE(index.fold("Filler", key));
  std::vector<Hit> whole;
  IndexCursor c1;
  ASSERT_TRUE(index.list(key, c1, 1000, whole));
  EXPECT_EQ(whole.size(), 400u);
  std::vector<Hit> pieces;
  IndexCursor c2;
  for (int guard = 0; guard < 1000 && !c2.done; guard++) {
    std::vector<Hit> page;
    ASSERT_TRUE(index.list(key, c2, 7, page));
    EXPECT_LE(page.size(), 7u);
    pieces.insert(pieces.end(), page.begin(), page.end());
  }
  ASSERT_EQ(pieces.size(), whole.size());
  for (size_t i = 0; i < whole.size(); i++) EXPECT_EQ(pieces[i].display, whole[i].display);
  Hit hit;
  ASSERT_TRUE(index.find("Beta history", hit));
  EXPECT_EQ(hit.article, 1u);
  EXPECT_TRUE(hit.isRedirect());
  EXPECT_FALSE(index.find("Beta histor", hit));
}

// ------------------------------------------------------------------ raster

namespace {
struct Canvas {
  int w, h;
  std::vector<int> px;
  Canvas(int w_, int h_) : w(w_), h(h_), px(w_ * h_, 0) {}
  static void put(void* ctx, int y, int x, const uint8_t* cov, int n) {
    auto* c = static_cast<Canvas*>(ctx);
    for (int i = 0; i < n; i++) {
      const int xx = x + i;
      if (y < 0 || y >= c->h || xx < 0 || xx >= c->w) continue;
      c->px[y * c->w + xx] = std::max(c->px[y * c->w + xx], static_cast<int>(cov[i]));
    }
  }
  int at(int x, int y) const { return px[y * w + x]; }
  double ink() const {
    double s = 0;
    for (int v : px) s += v / 255.0;
    return s;
  }
};

void rect(Rasterizer& r, float x0, float y0, float x1, float y1, bool clockwise = true) {
  r.moveTo(x0, y0);
  if (clockwise) {
    r.lineTo(x1, y0);
    r.lineTo(x1, y1);
    r.lineTo(x0, y1);
  } else {
    r.lineTo(x0, y1);
    r.lineTo(x1, y1);
    r.lineTo(x1, y0);
  }
  r.close();
}
}  // namespace

TEST(XwaRaster, PixelAlignedSquareIsSolid) {
  Canvas c(8, 8);
  Rasterizer r;
  ASSERT_TRUE(r.begin(0, 0, 8, 8));
  rect(r, 2, 2, 6, 6);
  r.fill(Canvas::put, &c);
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      const bool in = x >= 2 && x < 6 && y >= 2 && y < 6;
      EXPECT_EQ(c.at(x, y), in ? 255 : 0) << x << "," << y;
    }
  }
}

TEST(XwaRaster, HalfPixelEdgesAreGrey) {
  Canvas c(8, 8);
  Rasterizer r;
  ASSERT_TRUE(r.begin(0, 0, 8, 8));
  rect(r, 2.5f, 2.5f, 5.5f, 5.5f);
  r.fill(Canvas::put, &c);
  EXPECT_NEAR(c.at(2, 2), 64, 1);   // corner: a quarter
  EXPECT_NEAR(c.at(3, 2), 128, 1);  // edge: a half
  EXPECT_EQ(c.at(3, 3), 255);
  EXPECT_NEAR(c.at(5, 5), 64, 1);
  EXPECT_NEAR(c.ink(), 9.0, 0.02);
}

TEST(XwaRaster, NonzeroWinding) {
  // An inner square wound the other way is a hole...
  Canvas hole(10, 10);
  Rasterizer r;
  ASSERT_TRUE(r.begin(0, 0, 10, 10));
  rect(r, 1, 1, 9, 9, true);
  rect(r, 3, 3, 7, 7, false);
  r.fill(Canvas::put, &hole);
  EXPECT_EQ(hole.at(5, 5), 0);
  EXPECT_EQ(hole.at(2, 2), 255);
  // ...wound the same way, it stays filled (nonzero, not even-odd).
  Canvas same(10, 10);
  ASSERT_TRUE(r.begin(0, 0, 10, 10));
  rect(r, 1, 1, 9, 9, true);
  rect(r, 3, 3, 7, 7, true);
  r.fill(Canvas::put, &same);
  EXPECT_EQ(same.at(5, 5), 255);
}

TEST(XwaRaster, AreaIsExactAndClippedCleanly) {
  // A triangle partly outside the box on both sides: what lies inside is
  // drawn with the right total coverage.
  Canvas c(20, 20);
  Rasterizer r;
  ASSERT_TRUE(r.begin(0, 0, 20, 20));
  r.moveTo(-10.3f, 2.2f);
  r.lineTo(30.7f, 9.1f);
  r.lineTo(4.4f, 18.6f);
  r.close();
  r.fill(Canvas::put, &c);
  // The same triangle in a box that holds all of it, then cut to x 0..20.
  Canvas big(60, 20);
  ASSERT_TRUE(r.begin(-20, 0, 60, 20));
  r.moveTo(-10.3f, 2.2f);
  r.lineTo(30.7f, 9.1f);
  r.lineTo(4.4f, 18.6f);
  r.close();
  struct Shift {
    Canvas* c;
    static void put(void* ctx, int y, int x, const uint8_t* cov, int n) {
      Canvas::put(static_cast<Shift*>(ctx)->c, y, x, cov, n);
    }
  } shift{&big};
  r.fill(Shift::put, &shift);
  for (int y = 0; y < 20; y++) {
    for (int x = 0; x < 20; x++) EXPECT_NEAR(c.at(x, y), big.at(x, y), 1) << x << "," << y;
  }
  // Shoelace area of the whole triangle against the big box's ink.
  const double area = 0.5 * std::fabs((30.7 + 10.3) * (18.6 - 2.2) - (4.4 + 10.3) * (9.1 - 2.2));
  Canvas whole(60, 20);
  ASSERT_TRUE(r.begin(-20, 0, 60, 20));
  r.moveTo(-10.3f, 2.2f);
  r.lineTo(30.7f, 9.1f);
  r.lineTo(4.4f, 18.6f);
  r.close();
  struct Offset {
    Canvas* c;
    static void put(void* ctx, int y, int x, const uint8_t* cov, int n) {
      Canvas::put(static_cast<Offset*>(ctx)->c, y, x + 20, cov, n);
    }
  } offset{&whole};
  r.fill(Offset::put, &offset);
  EXPECT_NEAR(whole.ink(), area, area * 0.01);
}

TEST(XwaMathDraw, EveryFormulaInTheTestArticleDraws) {
  MemorySource src(loadFile(dataPath("rich-full.xwa")));
  Archive ar;
  ASSERT_EQ(ar.open(&src, kX4), Status::Ok);
  GlyphTable glyphs;
  ASSERT_TRUE(glyphs.open(ar));
  StreamReader r;
  ArticleHeader h;
  ASSERT_TRUE(r.open(ar, 0) && readHeader(r, h));
  ASSERT_TRUE(r.seek(h.bodyStart));
  int drawn = 0;
  BlockItem b;
  std::vector<uint8_t> buf;
  while (readBlock(r, h.bodyEnd(), b) && b.op != 0) {
    if (b.op == block_op::MATH_BLOCK) {
      Formula f;
      ASSERT_TRUE(loadFormula(r, b.dataPos, b.dataLen, buf, f));
      EXPECT_TRUE(drawable(f));
      const float ppu = 32.0f / glyphs.upem();  // a 32 px em
      const int w = static_cast<int>(f.width * ppu) + 8;
      const int asc = static_cast<int>(f.ascent * ppu) + 4;
      const int hgt = asc + static_cast<int>(f.descent * ppu) + 8;
      Canvas c(w, hgt);
      EXPECT_TRUE(drawFormula(f, glyphs, 2.0f, static_cast<float>(asc), ppu, Canvas::put, &c));
      EXPECT_GT(c.ink(), 4.0);
      drawn++;
    }
    ASSERT_TRUE(r.seek(b.next));
  }
  EXPECT_GT(drawn, 0);
}

// ------------------------------------------------------------------ damage

namespace {
// Reads everything an archive offers, as the firmware would, ignoring
// failures: damaged data must give errors, never crashes or hangs.
void walk(Archive& ar) {
  TitleIndex index(ar);
  for (const char* q : {"", "a", "ein", "filler 0", "한국", "zzz"}) {
    std::string key;
    if (!index.fold(q, key)) continue;
    IndexCursor c;
    std::vector<Hit> hits;
    index.list(key, c, 20, hits);
    hits.clear();
    index.popular(key, 8, hits);
    Hit h;
    index.find(q, h);
  }
  GlyphTable glyphs;
  const bool haveGlyphs = glyphs.open(ar);
  std::vector<uint8_t> buf;
  FallbackFont font;
  if (font.open(ar)) {
    std::vector<uint8_t> g;
    std::vector<uint8_t> bold;
    for (uint32_t cp = 0x250; cp < 0x2200; cp++) {
      for (int style = 0; style < 4; style++) {
        bool upright = false;
        StyledGlyph sg;
        if (!font.resolve(cp, style & 1, style & 2, upright, sg) || !sg.section->glyph(sg.glyph, g)) continue;
        if (sg.embolden && emboldenGlyph(g.data(), g.size(), 25, bold)) g.swap(bold);
        Rasterizer ras;
        Canvas c(32, 32);
        ras.begin(0, 0, 32, 32);
        outlineGlyph(g.data(), g.size(), ras, 4, 24, 0.02f, 0.02f, sg.shear ? FallbackFont::SHEAR : 0);
        ras.fill(Canvas::put, &c);
      }
    }
  }
  for (uint32_t id = 0; id < ar.articleCount() && id < 40; id++) {
    std::string t;
    ar.title(id, t);
    StreamReader r;
    ArticleHeader h;
    if (!r.open(ar, id) || !readHeader(r, h)) continue;
    r.seek(h.bodyStart);
    BlockItem b;
    for (int guard = 0; guard < 100000 && readBlock(r, h.bodyEnd(), b) && b.op != 0; guard++) {
      if (b.runLen) {
        r.seek(b.runPos);
        InlineItem it;
        bool damaged = false;
        while (readInline(r, b.runPos + b.runLen, it, damaged)) {
          if (it.kind == InlineItem::Math) {
            const uint32_t back = r.pos();
            Formula f;
            if (loadFormula(r, it.pos, it.len, buf, f) && haveGlyphs) {
              Canvas c(64, 64);
              drawFormula(f, glyphs, 0, 32, 0.02f, Canvas::put, &c);
            }
            r.seek(back);
          }
        }
      }
      if (!r.seek(b.next)) break;
    }
    for (uint32_t i = 0; i < h.tables.size(); i++) {
      TableInfo ti;
      readTable(r, h, i, ti);
    }
    std::vector<InfoboxRow> rows;
    readInfobox(r, h, rows);
  }
  for (uint32_t id = 0; id < ar.imageCount(); id++) {
    ImageRecord rec;
    if (!ar.imageRecord(id, rec) || rec.encoding != image_encoding::LINE_ART) continue;
    La2Reader la2;
    if (!la2.begin(ar, rec)) continue;
    std::vector<uint8_t> levels(la2.width());
    while (la2.row(levels.data())) {
    }
  }
  for (int d = 0; d < DAYS_IN_YEAR; d++) {
    uint32_t a;
    ar.dayArticle(d, a);
  }
}

uint32_t rng(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
}  // namespace

namespace {
// A square glyph, 0..100 font units, clockwise as TrueType draws outlines.
std::vector<uint8_t> squareGlyph(bool clockwise) {
  std::vector<uint8_t> g;
  auto put = [&](int v) {
    g.push_back(static_cast<uint8_t>(v & 0xFF));
    g.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  for (int v : {0, 0, 100, 100}) put(v);
  const int cw[4][2] = {{0, 0}, {0, 100}, {100, 100}, {100, 0}};
  for (int i = 0; i < 4; i++) {
    const int k = clockwise ? i : (4 - i) % 4;
    g.push_back(i == 0 ? 0 : 1);
    put(cw[k][0]);
    put(cw[k][1]);
  }
  g.push_back(4);
  g.push_back(5);
  return g;
}

double drawGlyph(const std::vector<uint8_t>& g, float shear, Canvas& c, float scale) {
  Rasterizer r;
  EXPECT_TRUE(r.begin(0, 0, c.w, c.h));
  EXPECT_TRUE(outlineGlyph(g.data(), g.size(), r, c.w / 4.0f, c.h * 3 / 4.0f, scale, scale, shear));
  r.fill(Canvas::put, &c);
  return c.ink();
}
}  // namespace

TEST(XwaFont, StylesFollowTheSpec) {
  MemorySource src(loadFile(dataPath("rich-full.xwa")));
  Archive ar;
  ASSERT_EQ(ar.open(&src, kX4), Status::Ok);
  FallbackFont f;
  ASSERT_TRUE(f.open(ar));
  EXPECT_TRUE(f.hasBold());
  EXPECT_TRUE(f.hasItalic());
  EXPECT_TRUE(f.hasBoldItalic());
  EXPECT_TRUE(f.has(0x03B1));   // α
  EXPECT_FALSE(f.has('a'));     // built in
  EXPECT_FALSE(f.has(0x4E00));  // not in the text
  struct Case {
    uint32_t cp;
    bool bold, italic;
    uint32_t section;  // where the glyph comes from
    bool embolden, shear;
  };
  const Case cases[] = {
      {0x03B1, false, false, section::FONT, false, false},            // α
      {0x03B1, true, false, section::FONT_BOLD, false, false},        // real bold
      {0x0395, true, false, section::FONT, true, false},              // Ε: never bold in the text
      {0x03B1, false, true, section::FONT_ITALIC, false, false},      // real italic
      {0x03C6, false, true, section::FONT, false, true},              // φ: left out of FONT_ITALIC
      {0x211D, false, true, section::FONT, false, false},             // ℝ stays upright
      {0x0915, false, true, section::FONT, false, false},             // Devanagari stays upright
      {0x03C8, true, true, section::FONT_BOLD_ITALIC, false, false},  // ψ: real bold italic
      {0x0395, true, true, section::FONT, true, true},                // Ε: all synthesised
  };
  for (const Case& c : cases) {
    bool upright = false;
    StyledGlyph g;
    ASSERT_TRUE(f.resolve(c.cp, c.bold, c.italic, upright, g)) << std::hex << c.cp;
    FontSection s;
    ASSERT_TRUE(s.open(ar, c.section));
    const int32_t want = s.find(c.cp);
    ASSERT_GE(want, 0) << std::hex << c.cp;
    EXPECT_EQ(g.glyph, static_cast<uint32_t>(want)) << std::hex << c.cp;
    EXPECT_EQ(g.section->glyphCount(), s.glyphCount()) << std::hex << c.cp;
    EXPECT_EQ(g.embolden, c.embolden) << std::hex << c.cp;
    EXPECT_EQ(g.shear, c.shear) << std::hex << c.cp;
    const uint16_t adv = s.advance(want);
    EXPECT_EQ(g.advance, c.embolden ? adv + 50 : adv) << std::hex << c.cp;
  }
  // Bold italic falls back to the bold glyph, sheared, when it has one.
  bool upright = false;
  StyledGlyph g;
  FontSection bold;
  ASSERT_TRUE(bold.open(ar, section::FONT_BOLD));
  ASSERT_GE(bold.find(0x03B1), 0);
  ASSERT_TRUE(f.resolve(0x03B1, true, true, upright, g));
  if (g.section->glyphCount() == bold.glyphCount() && !g.embolden) {
    EXPECT_TRUE(g.shear);
  }
}

TEST(XwaFont, UprightAndCombining) {
  EXPECT_FALSE(staysUpright('a'));
  EXPECT_FALSE(staysUpright(0x03C6));  // φ
  EXPECT_FALSE(staysUpright(0x0416));  // Cyrillic
  EXPECT_TRUE(staysUpright('+'));
  EXPECT_TRUE(staysUpright(0x00D7));   // ×
  EXPECT_TRUE(staysUpright(0x2211));   // ∑
  EXPECT_TRUE(staysUpright(0x05D0));   // Hebrew
  EXPECT_TRUE(staysUpright(0x0627));   // Arabic
  EXPECT_TRUE(staysUpright(0xFEFB));   // Lam-Alef
  EXPECT_TRUE(staysUpright(0x4E2D));   // Han
  EXPECT_TRUE(staysUpright(0xAC00));   // Hangul
  EXPECT_TRUE(staysUpright(0x20000));  // CJK extension B
  EXPECT_TRUE(isCombining(0x0301));
  EXPECT_TRUE(isCombining(0x20D7));
  EXPECT_FALSE(isCombining('a'));
}

TEST(XwaFont, EmboldenMovesOutlinesOutward) {
  for (const bool clockwise : {true, false}) {
    const auto square = squareGlyph(clockwise);
    std::vector<uint8_t> bold;
    ASSERT_TRUE(emboldenGlyph(square.data(), square.size(), 10, bold));
    // bbox and every corner move out by 10 on both axes.
    EXPECT_EQ(static_cast<int16_t>(le16(bold.data())), -10);
    EXPECT_EQ(static_cast<int16_t>(le16(bold.data() + 6)), 110);
    Canvas a(64, 64);
    Canvas b(64, 64);
    const double regular = drawGlyph(square, 0, a, 0.2f);  // 20 px square
    const double heavy = drawGlyph(bold, 0, b, 0.2f);      // 24 px square
    EXPECT_NEAR(regular, 400, 1) << clockwise;
    EXPECT_NEAR(heavy, 576, 2) << clockwise;
  }
}

TEST(XwaFont, EmboldenKeepsHoles) {
  MemorySource src(loadFile(dataPath("rich-full.xwa")));
  Archive ar;
  ASSERT_EQ(ar.open(&src, kX4), Status::Ok);
  FontSection f;
  ASSERT_TRUE(f.open(ar, section::FONT));
  const int32_t o = f.find(0x03BF);  // ο, a ring
  ASSERT_GE(o, 0);
  std::vector<uint8_t> g;
  ASSERT_TRUE(f.glyph(o, g));
  std::vector<uint8_t> bold;
  ASSERT_TRUE(emboldenGlyph(g.data(), g.size(), 25, bold));
  Canvas a(160, 160);
  Canvas b(160, 160);
  const double regular = drawGlyph(g, 0, a, 0.1f);
  const double heavy = drawGlyph(bold, 0, b, 0.1f);
  EXPECT_GT(heavy, regular * 1.15);
  // The counter shrinks but stays open: the middle of the ring is paper.
  const int cx = 40 + 30;   // origin 40, ring centre about 300 units right
  const int cy = 120 - 27;  // and 270 units up
  EXPECT_EQ(a.at(cx, cy), 0);
  EXPECT_EQ(b.at(cx, cy), 0);
}

TEST(XwaFont, ShearSlantsRight) {
  const auto square = squareGlyph(true);
  Canvas a(80, 80);
  drawGlyph(square, FallbackFont::SHEAR, a, 0.3f);  // 30 px, origin (20, 60)
  auto firstInk = [&](int y) {
    for (int x = 0; x < a.w; x++) {
      if (a.at(x, y) > 128) return x;
    }
    return -1;
  };
  // The bottom row starts at the origin; the top, 30 px up, ~6 px right.
  EXPECT_NEAR(firstInk(59), 20, 1);
  EXPECT_NEAR(firstInk(31), 26, 1);
}

TEST(XwaDamage, RandomDamageNeverCrashes) {
  for (const char* name : {"rich-compact.xwa", "rich-full.xwa"}) {
    const auto good = loadFile(dataPath(name));
    uint32_t seed = 0x9E3779B9u;
    for (int round = 0; round < 300; round++) {
      auto bytes = good;
      const int flips = 1 + static_cast<int>(rng(seed) % 16);
      for (int i = 0; i < flips; i++) {
        // Mostly past the superblock, so the damage reaches the sections.
        const size_t at = 4096 + rng(seed) % (bytes.size() - 4096);
        bytes[at] ^= static_cast<uint8_t>(1u << (rng(seed) % 8));
      }
      if (round % 10 == 0) {
        bytes[rng(seed) % 4092] ^= 0xFF;  // superblock too, resealed
        resealSuperblock(bytes);
      }
      MemorySource src(std::move(bytes));
      Archive ar;
      if (ar.open(&src, round % 2 ? kX4 : kPro) == Status::Ok) walk(ar);
    }
  }
}

TEST(XwaDamage, RandomFormulaBytesNeverCrash) {
  MemorySource src(loadFile(dataPath("rich-full.xwa")));
  Archive ar;
  ASSERT_EQ(ar.open(&src, kX4), Status::Ok);
  GlyphTable glyphs;
  ASSERT_TRUE(glyphs.open(ar));
  uint32_t seed = 12345;
  std::vector<uint8_t> data;
  for (int round = 0; round < 3000; round++) {
    data.resize(rng(seed) % 64);
    for (auto& b : data) b = static_cast<uint8_t>(rng(seed));
    if (!data.empty()) data[0] &= 3;
    Formula f;
    if (!parseFormula(data.data(), data.size(), f)) continue;
    drawable(f);
    Canvas c(40, 40);
    drawFormula(f, glyphs, 5, 20, 0.02f, Canvas::put, &c);
  }
}
