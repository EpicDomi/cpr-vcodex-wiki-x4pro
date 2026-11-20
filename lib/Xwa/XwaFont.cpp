#include "XwaFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xwa {

// ------------------------------------------------------------------ section

bool FontSection::open(Archive& archive, uint32_t type) {
  ar_ = nullptr;
  sparse_.clear();
  for (Hit& h : cache_) h = Hit();
  const SectionInfo& s = archive.sectionInfo(type);
  if (!s.present() || s.length < 16) return false;
  uint8_t head[12];
  if (!archive.readCached(s.offset, head, sizeof(head))) return false;
  upem_ = le16(head);
  ascender_ = static_cast<int16_t>(le16(head + 2));
  descender_ = static_cast<int16_t>(le16(head + 4));
  rangeCount_ = le32(head + 8);
  const uint64_t countAt = 12ull + 12ull * rangeCount_;
  uint8_t n[4];
  if (upem_ == 0 || countAt + 4 > s.length || !archive.readCached(s.offset + countAt, n, 4)) return false;
  glyphCount_ = le32(n);
  advancesAt_ = countAt + 4;
  offsetsAt_ = advancesAt_ + 2ull * glyphCount_;
  dataAt_ = offsetsAt_ + 4ull * (glyphCount_ + 1ull);
  if (dataAt_ > s.length) return false;
  base_ = s.offset;
  length_ = s.length;
  ar_ = &archive;
  // Check the ranges once (sorted, inside the glyph table) and keep every
  // 64th first code point for lookups.
  sparse_.reserve((rangeCount_ + GROUP - 1) / GROUP);
  uint32_t prevLast = 0;
  for (uint32_t i = 0; i < rangeCount_; i++) {
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t glyph = 0;
    if (!range(i, first, last, glyph) || last < first || (i > 0 && first <= prevLast) || last > 0x10FFFF ||
        glyph >= glyphCount_ || last - first >= glyphCount_ - glyph) {
      ar_ = nullptr;
      sparse_.clear();
      return false;
    }
    if (i % GROUP == 0) sparse_.push_back(first);
    prevLast = last;
  }
  return true;
}

bool FontSection::range(uint32_t i, uint32_t& first, uint32_t& last, uint32_t& glyph) {
  uint8_t r[12];
  if (!ar_ || i >= rangeCount_ || !ar_->readCached(base_ + 12 + 12ull * i, r, sizeof(r))) return false;
  first = le32(r);
  last = le32(r + 4);
  glyph = le32(r + 8);
  return true;
}

int32_t FontSection::find(uint32_t cp) {
  if (!ar_ || sparse_.empty()) return -1;
  Hit& hit = cache_[cp % CACHE];
  if (hit.cp == cp) return hit.glyph;
  int32_t result = -1;
  const auto it = std::upper_bound(sparse_.begin(), sparse_.end(), cp);
  if (it != sparse_.begin()) {
    const uint32_t group = static_cast<uint32_t>(it - sparse_.begin()) - 1;
    const uint32_t from = group * GROUP;
    const uint32_t count = std::min(GROUP, rangeCount_ - from);
    uint8_t buf[GROUP * 12];
    if (ar_->readCached(base_ + 12 + 12ull * from, buf, 12ull * count)) {
      // The last range starting at or before cp.
      uint32_t lo = 0;
      uint32_t hi = count;
      while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (le32(buf + 12 * mid) <= cp) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      const uint8_t* r = buf + 12 * lo;
      if (le32(r) <= cp && cp <= le32(r + 4)) result = static_cast<int32_t>(le32(r + 8) + (cp - le32(r)));
    }
  }
  hit.cp = cp;
  hit.glyph = result;
  return result;
}

uint16_t FontSection::advance(uint32_t glyph) {
  uint8_t a[2];
  if (!ar_ || glyph >= glyphCount_ || !ar_->readCached(base_ + advancesAt_ + 2ull * glyph, a, 2)) return 0;
  return le16(a);
}

bool FontSection::glyph(uint32_t glyph, std::vector<uint8_t>& out) {
  if (!ar_ || glyph >= glyphCount_) return false;
  uint8_t o[8];
  if (!ar_->readCached(base_ + offsetsAt_ + 4ull * glyph, o, 8)) return false;
  const uint32_t a = le32(o);
  const uint32_t b = le32(o + 4);
  if (b < a || b - a < 9 || b - a > 65536 || dataAt_ + b > length_) return false;
  out.resize(b - a);
  return ar_->readCached(base_ + dataAt_ + a, out.data(), out.size());
}

// ------------------------------------------------------------------ styles

bool FallbackFont::open(Archive& archive) {
  if (!regular_.open(archive, section::FONT)) return false;
  bold_.open(archive, section::FONT_BOLD);
  italic_.open(archive, section::FONT_ITALIC);
  boldItalic_.open(archive, section::FONT_BOLD_ITALIC);
  return true;
}

bool FallbackFont::resolve(uint32_t cp, bool bold, bool italic, bool& prevUpright, StyledGlyph& out) {
  out = StyledGlyph();
  const int32_t regular = regular_.find(cp);
  if (regular < 0) return false;
  bool upright = true;
  if (italic) {
    if (isCombining(cp)) {
      upright = prevUpright;
    } else {
      upright = staysUpright(cp);
      prevUpright = upright;
    }
  }
  auto take = [&](FontSection& s) -> bool {
    const int32_t g = s.find(cp);
    if (g < 0) return false;
    out.section = &s;
    out.glyph = static_cast<uint32_t>(g);
    return true;
  };
  if (bold && italic) {
    if (!take(boldItalic_)) {
      if (!take(bold_)) {
        take(regular_);
        out.embolden = true;
      }
      out.shear = !upright;
    }
  } else if (bold) {
    if (!take(bold_)) {
      take(regular_);
      out.embolden = true;
    }
  } else if (italic) {
    if (!take(italic_)) {
      take(regular_);
      out.shear = !upright;
    }
  } else {
    take(regular_);
  }
  out.advance = out.section->advance(out.glyph);
  if (out.embolden && out.advance > 0) {
    out.advance = static_cast<uint16_t>(
        std::min<uint32_t>(0xFFFF, out.advance + static_cast<uint32_t>(std::lround(2.0f * BOLD_OFFSET_EM * upem()))));
  }
  return true;
}

// ------------------------------------------------------------------ upright

namespace {

struct Range {
  uint32_t first, last;
};

// Sorted, not overlapping. Scripts without an italic convention, symbols
// that do not slant, and the characters spec 10.1 lists as drawn upright
// by Noto Sans Italic.
constexpr Range UPRIGHT[] = {
    {0x002B, 0x002B},
    {0x003C, 0x003C},
    {0x003E, 0x003E},
    {0x005E, 0x005E},
    {0x007C, 0x007C},
    {0x00A4, 0x00A4},
    {0x00A6, 0x00A6},
    {0x00A9, 0x00A9},
    {0x00AE, 0x00AE},
    {0x00B0, 0x00B1},
    {0x00B6, 0x00B6},
    {0x00D7, 0x00D7},
    {0x00F7, 0x00F7},
    {0x02C2, 0x02C5},
    {0x02DA, 0x02DA},
    {0x02E5, 0x02EB},
    {0x02F9, 0x02FC},
    {0x02FF, 0x02FF},
    {0x0482, 0x0482},
    {0x0590, 0x08FF},  // Hebrew, Arabic, Syriac, Thaana, NKo, Samaritan, Mandaic
    {0x0900, 0x0DFF},  // Devanagari to Sinhala
    {0x0E00, 0x0FFF},  // Thai, Lao, Tibetan
    {0x1000, 0x109F},  // Myanmar
    {0x1100, 0x11FF},  // Hangul Jamo
    {0x1200, 0x139F},  // Ethiopic
    {0x1400, 0x167F},  // Canadian syllabics
    {0x1700, 0x1AAF},  // Philippine scripts, Khmer, Mongolian, Limbu, Tai Le, Tai Tham
    {0x1B00, 0x1C7F},  // Balinese, Sundanese, Batak, Lepcha, Ol Chiki
    {0x1CD0, 0x1CFF},  // Vedic extensions
    {0x1D11, 0x1D13},
    {0x1D1D, 0x1D1F},
    {0x1D59, 0x1D59},
    {0x1FBD, 0x1FBD},
    {0x1FBF, 0x1FBF},
    {0x2016, 0x2016},
    {0x2032, 0x2034},
    {0x203B, 0x203B},
    {0x204A, 0x204D},
    {0x2052, 0x2052},
    {0x2055, 0x205E},
    {0x207A, 0x207A},
    {0x208A, 0x208A},
    {0x20AF, 0x20AF},
    {0x2102, 0x2102},
    {0x2104, 0x2104},
    {0x210A, 0x210D},
    {0x2110, 0x2113},
    {0x2115, 0x2115},
    {0x2117, 0x211D},
    {0x2120, 0x2122},
    {0x2124, 0x2124},
    {0x2128, 0x2128},
    {0x212C, 0x2131},
    {0x2133, 0x2138},
    {0x213C, 0x2140},
    {0x2145, 0x214A},
    {0x214C, 0x214C},
    {0x214F, 0x214F},
    {0x2190, 0x2BFF},  // arrows, mathematical operators, technical, shapes, symbols, dingbats, braille
    {0x2E00, 0x2E0E},
    {0x2E12, 0x2E17},
    {0x2E19, 0x2E19},
    {0x2E1B, 0x2E1E},
    {0x2E20, 0x2E27},
    {0x2E2A, 0x2E2D},
    {0x2E30, 0x2E30},
    {0x2E3D, 0x2E3F},
    {0x2E41, 0x2E41},
    {0x2E5D, 0x2E5D},
    {0x2E80, 0xA4CF},  // CJK, kana, bopomofo, Hangul, Yi
    {0xA673, 0xA673},
    {0xA700, 0xA719},
    {0xA71B, 0xA71C},
    {0xA720, 0xA721},
    {0xA800, 0xAB2F},  // Syloti Nagri to Ethiopic extended
    {0xAB32, 0xAB32},
    {0xAB3D, 0xAB3E},
    {0xABC0, 0xD7FF},  // Meetei Mayek, Hangul syllables
    {0xF900, 0xFAFF},  // CJK compatibility ideographs
    {0xFB1D, 0xFDFF},  // Hebrew and Arabic presentation forms
    {0xFE10, 0xFE1F},  // vertical forms
    {0xFE30, 0xFFEF},  // CJK compatibility forms, small forms, Arabic forms B, full width
    {0xFFFC, 0xFFFD},
    {0x11000, 0x11FFF},  // Brahmi and the other historic Indic scripts
    {0x1D000, 0x1D24F},  // musical symbols
    {0x1D400, 0x1D7FF},  // mathematical letters (already styled)
    {0x1E800, 0x1EFFF},  // Mende Kikakui, Adlam, Arabic mathematical symbols
    {0x1F000, 0x1FAFF},  // symbols and emoji
    {0x20000, 0x3FFFF},  // CJK extensions
};

constexpr Range COMBINING[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0xFE20, 0xFE2F},
};

template <size_t N>
bool inRanges(const Range (&ranges)[N], uint32_t cp) {
  const Range* end = ranges + N;
  const Range* it = std::upper_bound(ranges, end, cp, [](uint32_t v, const Range& r) { return v < r.first; });
  return it != ranges && cp <= (it - 1)->last;
}

}  // namespace

bool staysUpright(uint32_t cp) { return inRanges(UPRIGHT, cp); }
bool isCombining(uint32_t cp) { return inRanges(COMBINING, cp); }

// ------------------------------------------------------------------ embolden

namespace {
struct Pt {
  float x, y;
};
}  // namespace

bool emboldenGlyph(const uint8_t* data, size_t len, float offset, std::vector<uint8_t>& out) {
  if (len < 9) return false;
  // Commands and their points, in order.
  static const uint8_t COUNT[4] = {1, 1, 2, 3};
  std::vector<uint8_t> cmds;
  std::vector<Pt> pts;
  const uint8_t* p = data + 8;
  const uint8_t* end = data + len;
  bool ended = false;
  while (p < end) {
    const uint8_t cmd = *p++;
    cmds.push_back(cmd);
    if (cmd == 5) {
      ended = true;
      break;
    }
    if (cmd == 4) continue;
    if (cmd > 3 || end - p < 4 * COUNT[cmd]) return false;
    for (int i = 0; i < COUNT[cmd]; i++) {
      pts.push_back(
          {static_cast<float>(static_cast<int16_t>(le16(p))), static_cast<float>(static_cast<int16_t>(le16(p + 2)))});
      p += 4;
    }
  }
  if (!ended) return false;

  // Contours: runs of points from one MOVE to the next.
  std::vector<std::pair<size_t, size_t>> contours;
  {
    size_t at = 0;
    for (uint8_t cmd : cmds) {
      if (cmd > 3) continue;
      if (cmd == 0) contours.emplace_back(at, at);
      if (!contours.empty()) contours.back().second = at + COUNT[cmd];
      at += COUNT[cmd];
    }
  }
  // Which way outlines run: the sign of the total area (outer contours
  // dominate holes).
  double area = 0;
  for (const auto& c : contours) {
    for (size_t i = c.first; i < c.second; i++) {
      const Pt& a = pts[i];
      const Pt& b = pts[i + 1 < c.second ? i + 1 : c.first];
      area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
  }
  // Outward normal of a direction (dx, dy): left of travel for clockwise
  // outlines (TrueType), right for counter-clockwise.
  const float side = area < 0 ? 1.0f : -1.0f;
  std::vector<Pt> moved(pts);
  for (const auto& c : contours) {
    const size_t n = c.second - c.first;
    if (n < 2) continue;
    auto at = [&](size_t k) -> const Pt& { return pts[c.first + (k % n)]; };
    for (size_t i = 0; i < n; i++) {
      const Pt& cur = at(i);
      // The nearest different points before and after.
      size_t back = 1;
      while (back < n && at(i + n - back).x == cur.x && at(i + n - back).y == cur.y) back++;
      size_t fwd = 1;
      while (fwd < n && at(i + fwd).x == cur.x && at(i + fwd).y == cur.y) fwd++;
      if (back >= n || fwd >= n) continue;
      const Pt& prev = at(i + n - back);
      const Pt& next = at(i + fwd);
      float ix = cur.x - prev.x;
      float iy = cur.y - prev.y;
      float ox = next.x - cur.x;
      float oy = next.y - cur.y;
      const float il = std::sqrt(ix * ix + iy * iy);
      const float ol = std::sqrt(ox * ox + oy * oy);
      ix /= il;
      iy /= il;
      ox /= ol;
      oy /= ol;
      // Normals of the edges in and out, and the mitre between them.
      const float nx = side * (-iy - oy);
      const float ny = side * (ix + ox);
      const float d = ix * ox + iy * oy;
      const float k = offset / std::max(1.0f + d, 0.25f);
      moved[c.first + i] = {cur.x + nx * k, cur.y + ny * k};
    }
  }

  auto put16 = [&](float v) {
    const long r = std::lround(v);
    const int16_t s = static_cast<int16_t>(std::max<long>(INT16_MIN, std::min<long>(INT16_MAX, r)));
    out.push_back(static_cast<uint8_t>(s & 0xFF));
    out.push_back(static_cast<uint8_t>((static_cast<uint16_t>(s) >> 8) & 0xFF));
  };
  out.clear();
  out.reserve(len + 8);
  const bool empty = pts.empty();
  for (int i = 0; i < 4; i++) {
    const float v = static_cast<float>(static_cast<int16_t>(le16(data + 2 * i)));
    put16(empty ? v : (i < 2 ? v - offset : v + offset));
  }
  size_t at = 0;
  for (uint8_t cmd : cmds) {
    out.push_back(cmd);
    if (cmd > 3) continue;
    for (int i = 0; i < COUNT[cmd]; i++) {
      put16(moved[at].x);
      put16(moved[at].y);
      at++;
    }
  }
  return true;
}

}  // namespace xwa
