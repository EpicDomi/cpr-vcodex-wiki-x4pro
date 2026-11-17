#include "WikiLayout.h"

#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <XwaMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "ReaderFontSizes.h"
#include "WikiSession.h"
#include "fontIds.h"

namespace wiki {

namespace {

// ------------------------------------------------------------------ fonts

int builtinFont(bool sans, uint8_t pt) {
  switch (pt) {
    case 10:
      return sans ? NOTOSANS_10_FONT_ID : BOOKERLY_10_FONT_ID;
    case 12:
      return sans ? NOTOSANS_12_FONT_ID : BOOKERLY_12_FONT_ID;
    case 16:
      return sans ? NOTOSANS_16_FONT_ID : BOOKERLY_16_FONT_ID;
    case 18:
      return sans ? NOTOSANS_18_FONT_ID : BOOKERLY_18_FONT_ID;
    default:
      return sans ? NOTOSANS_14_FONT_ID : BOOKERLY_14_FONT_ID;
  }
}

// The fonts are drawn at their point size on a 150 dpi grid.
constexpr float PX_PER_PT = 150.0f / 72.0f;

// ------------------------------------------------------------------ text

bool isCjk(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
         (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// Closing punctuation and small kana never start a line; opening
// punctuation never ends one.
bool noBreakBefore(uint32_t cp) {
  switch (cp) {
    case 0x3001:
    case 0x3002:
    case 0x3005:
    case 0x3009:
    case 0x300B:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x3015:
    case 0x30FC:
    case 0xFF01:
    case 0xFF09:
    case 0xFF0C:
    case 0xFF0E:
    case 0xFF1A:
    case 0xFF1B:
    case 0xFF1F:
      return true;
    default:
      return (cp >= 0x3041 && cp <= 0x3049 && (cp & 1)) || cp == 0x3063 || cp == 0x30C3;
  }
}

bool noBreakAfter(uint32_t cp) {
  switch (cp) {
    case 0x3008:
    case 0x300A:
    case 0x300C:
    case 0x300E:
    case 0x3010:
    case 0x3014:
    case 0xFF08:
      return true;
    default:
      return false;
  }
}

uint8_t styleBits(const uint8_t counts[6]) {
  uint8_t s = EpdFontFamily::REGULAR;
  if (counts[0]) s |= EpdFontFamily::BOLD;
  if (counts[1]) s |= EpdFontFamily::ITALIC;
  if (counts[3]) {
    s |= EpdFontFamily::SUP;
  } else if (counts[2]) {
    s |= EpdFontFamily::SUB;
  }
  return s;
}

}  // namespace

TextStyle TextStyle::fromSettings(const GfxRenderer& renderer) {
  TextStyle s;
  const int body = SETTINGS.getReaderFontId();
  s.id[FONT_BODY] = s.id[FONT_SMALL] = s.id[FONT_LARGE] = body;
  float bodyPt = SETTINGS.fontPointSize;
  float smallPt = bodyPt;
  float largePt = bodyPt;
  const bool sdFont =
      SETTINGS.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver &&
      SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize) == body;
  if (!sdFont) {
    // Built-in families come at five sizes: captions and tables one size
    // down, section headings one up.
    const bool sans = SETTINGS.fontFamily == CrossPointSettings::NOTOSANS;
    const size_t n = std::size(BUILTIN_READER_POINT_SIZES);
    const uint8_t pt = snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, n, SETTINGS.fontPointSize);
    size_t i = 0;
    while (i + 1 < n && BUILTIN_READER_POINT_SIZES[i] != pt) i++;
    const uint8_t small = BUILTIN_READER_POINT_SIZES[i > 0 ? i - 1 : 0];
    const uint8_t large = BUILTIN_READER_POINT_SIZES[i + 1 < n ? i + 1 : i];
    s.id[FONT_SMALL] = builtinFont(sans, small);
    s.id[FONT_LARGE] = builtinFont(sans, large);
    bodyPt = pt;
    smallPt = small;
    largePt = large;
  }
  const float compression = SETTINGS.getReaderLineCompression();
  const float pts[FONT_SLOTS] = {bodyPt, smallPt, largePt};
  for (int i = 0; i < FONT_SLOTS; i++) {
    s.lineHeight[i] = static_cast<int16_t>(std::max(1, renderer.getLineHeight(s.id[i], compression)));
    s.ascent[i] = static_cast<int16_t>(renderer.getFontAscenderSize(s.id[i]));
    s.em[i] = pts[i] * PX_PER_PT;
  }
  s.tracking = SETTINGS.getCharacterSpacing();
  s.wordSpacing = SETTINGS.wordSpacing ? SETTINGS.wordSpacing : 100;
  s.justify = SETTINGS.paragraphAlignment == CrossPointSettings::JUSTIFIED;
  s.hyphenate = SETTINGS.hyphenationEnabled != 0;
  s.extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0;
  return s;
}

void Page::clear() {
  items.clear();
  areas.clear();
  text.clear();
  start = Cursor();
  startPos = endPos = 0;
}

void BlockLines::clear() {
  lines.clear();
  items.clear();
  areas.clear();
  text.clear();
}

void BlockLines::copyItems(const BlockLines& src, const Line& line, int16_t dx, int16_t dy) {
  for (uint16_t i = 0; i < line.items; i++) {
    DrawItem it = src.items[line.item + i];
    it.x = static_cast<int16_t>(it.x + dx);
    it.y = static_cast<int16_t>(it.y + dy);
    if (it.kind == DrawItem::Text) {
      const uint32_t from = it.a;
      it.a = static_cast<uint32_t>(text.size());
      text.append(src.text.c_str() + from, it.b);
      text.push_back('\0');
    }
    items.push_back(it);
  }
  for (uint16_t i = 0; i < line.areas; i++) {
    HitArea a = src.areas[line.area + i];
    a.x = static_cast<int16_t>(a.x + dx);
    a.y = static_cast<int16_t>(a.y + dy);
    areas.push_back(a);
  }
}

Line& BlockLines::open(int16_t h) {
  Line ln;
  ln.item = static_cast<uint16_t>(items.size());
  ln.area = static_cast<uint16_t>(areas.size());
  ln.h = h;
  lines.push_back(ln);
  return lines.back();
}

void BlockLines::close() {
  Line& ln = lines.back();
  ln.items = static_cast<uint16_t>(items.size() - ln.item);
  ln.areas = static_cast<uint16_t>(areas.size() - ln.area);
}

void BlockLines::append(const BlockLines& src, int16_t dx) {
  for (const Line& l : src.lines) {
    Line& n = open(l.h);
    n.base = l.base;
    n.gap = l.gap;
    n.keepWithNext = l.keepWithNext;
    copyItems(src, l, dx, 0);
    close();
  }
}

void zipColumns(const BlockLines* columns, const int16_t* xs, int count, BlockLines& out) {
  size_t rows = 0;
  for (int c = 0; c < count; c++) rows = std::max(rows, columns[c].lines.size());
  for (size_t k = 0; k < rows; k++) {
    int16_t base = 0;
    for (int c = 0; c < count; c++) {
      if (k < columns[c].lines.size()) base = std::max(base, columns[c].lines[k].base);
    }
    Line& ln = out.open(0);
    ln.base = base;
    int16_t h = 0;
    for (int c = 0; c < count; c++) {
      if (k >= columns[c].lines.size()) continue;
      const Line& src = columns[c].lines[k];
      const int16_t dy = static_cast<int16_t>(base - src.base);
      out.copyItems(columns[c], src, xs[c], dy);
      h = std::max<int16_t>(h, static_cast<int16_t>(src.h + dy));
    }
    out.lines.back().h = h;
    out.close();
  }
}

std::string runText(xwa::StreamReader& r, uint32_t pos, uint32_t len, size_t maxBytes) {
  std::string out;
  if (!r.seek(pos)) return out;
  xwa::InlineItem it;
  bool damaged = false;
  while (out.size() < maxBytes && xwa::readInline(r, pos + len, it, damaged)) {
    if (it.kind == xwa::InlineItem::Break) {
      out += ' ';
    } else if (it.kind == xwa::InlineItem::Text) {
      const uint32_t back = r.pos();
      const size_t at = out.size();
      out.resize(at + it.len);
      if (!r.seek(it.pos) || !r.bytes(&out[at], it.len)) out.resize(at);
      r.seek(back);
    }
  }
  if (out.size() > maxBytes) {
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80) cut--;
    out.resize(cut);
    out += "\xE2\x80\xA6";  // …
  }
  return out;
}

// ------------------------------------------------------------------ flow

// Lays out inline runs into lines, a word at a time: only the line being
// filled is held, so a paragraph of any length fits in RAM.
class Flow {
 public:
  Flow(Layout& layout, xwa::StreamReader& r) : L_(layout), r_(r), rend_(layout.renderer_), st_(layout.style_) {}

  // align: 0 the reader's alignment, 1 start, 2 centre.
  void begin(int16_t x, int16_t width, uint8_t align, BlockLines* out) {
    x_ = x;
    width_ = std::max<int16_t>(width, 8);
    align_ = align;
    out_ = out;
    line_.clear();
    word_.clear();
    text_.clear();
    links_.clear();
    pendingSpace_ = false;
    pendingGlue_ = false;
    rtlKnown_ = false;
    rtl_ = false;
    lineFont_ = FONT_BODY;
  }

  bool run(uint32_t pos, uint32_t len, uint8_t font, uint8_t baseStyle) {
    lineFont_ = font;
    if (!r_.seek(pos)) return false;
    uint8_t counts[6] = {};
    uint8_t stack[xwa::MAX_DEPTH];
    int depth = 0;
    uint16_t link = 0;
    uint16_t linkStack[xwa::MAX_DEPTH];
    int linkDepth = 0;
    xwa::InlineItem it;
    bool damaged = false;
    std::string buf;
    while (xwa::readInline(r_, pos + len, it, damaged)) {
      const uint8_t style = static_cast<uint8_t>(baseStyle | styleBits(counts));
      switch (it.kind) {
        case xwa::InlineItem::Text: {
          const uint32_t back = r_.pos();
          buf.resize(it.len);
          if (!r_.seek(it.pos) || !r_.bytes(&buf[0], it.len) || !r_.seek(back)) return false;
          addText(buf.data(), buf.size(), font, style, link);
          break;
        }
        case xwa::InlineItem::StylePush:
          if (depth < xwa::MAX_DEPTH) {
            stack[depth++] = it.style;
            if (it.style < 6) counts[it.style]++;
          }
          break;
        case xwa::InlineItem::StylePop:
          if (depth > 0) {
            const uint8_t s = stack[--depth];
            if (s < 6 && counts[s]) counts[s]--;
          }
          break;
        case xwa::InlineItem::Link:
          if (linkDepth < xwa::MAX_DEPTH) linkStack[linkDepth++] = link;
          links_.push_back(it);
          link = static_cast<uint16_t>(links_.size());
          break;
        case xwa::InlineItem::LinkEnd:
          link = linkDepth > 0 ? linkStack[--linkDepth] : 0;
          break;
        case xwa::InlineItem::Math:
          addMath(it.pos, it.len, font, link);
          break;
        case xwa::InlineItem::Icon:
          addIcon(it.value, it.height, font, link);
          break;
        case xwa::InlineItem::Break:
          commitWord();
          emitLine(true);
          pendingSpace_ = false;
          break;
        case xwa::InlineItem::NoteRef: {
          char note[16];
          snprintf(note, sizeof(note), "[%u]", static_cast<unsigned>(it.value));
          addText(note, std::strlen(note), font, static_cast<uint8_t>(style | EpdFontFamily::SUP), 0);
          break;
        }
      }
    }
    return !damaged;
  }

  // Generated text (labels, numbers).
  void text(const char* s, uint8_t font, uint8_t style) {
    lineFont_ = font;
    addText(s, std::strlen(s), font, style, 0);
  }

  void finish() {
    commitWord();
    if (!line_.empty()) emitLine(true);
  }

 private:
  struct Atom {
    enum Kind : uint8_t { Text, Math, Icon };
    Kind kind = Text;
    uint8_t font = FONT_BODY;
    uint8_t style = 0;
    bool space = false;  // a breakable space comes before it
    bool brk = false;    // a break may come before it, with no space (CJK)
    bool glue = false;   // a no-break space comes before it, inside a word
    int16_t w = 0;
    int16_t up = 0;  // boxes: extent above and below the baseline
    int16_t down = 0;
    int16_t spaceW = 0;  // width of the space before it
    uint16_t link = 0;
    uint32_t a = 0;  // Text: offset in text_; Math: record pos; Icon: image id
    uint32_t b = 0;  // Text: length; Math: record length; Icon: image kind
  };

  int fontId(uint8_t slot) const { return st_.id[slot]; }

  int16_t spaceWidth(uint8_t font, uint8_t style) const {
    const int w = rend_.getSpaceWidth(fontId(font), static_cast<EpdFontFamily::Style>(style & 3));
    return static_cast<int16_t>((w * st_.wordSpacing + 50) / 100);
  }

  int16_t measure(const Atom& a) const {
    return static_cast<int16_t>(rend_.getTextAdvanceX(fontId(a.font), text_.c_str() + a.a,
                                                      static_cast<EpdFontFamily::Style>(a.style), st_.tracking));
  }

  // `text`, for text atoms, is stored only once the word before is placed:
  // placing it may end a line, which compacts the text store.
  void push(Atom a, const char* text = nullptr, size_t len = 0) {
    if (pendingSpace_) a.space = true;
    // A no-break space joins this atom to the word before it.
    if (pendingGlue_ && !a.space && !word_.empty()) a.glue = true;
    pendingSpace_ = false;
    pendingGlue_ = false;
    if (a.space || a.brk) commitWord();
    if (a.space || a.glue) {
      const Atom* prev = !word_.empty() ? &word_.back() : !line_.empty() ? &line_.back() : nullptr;
      a.spaceW = prev ? spaceWidth(prev->font, prev->style) : spaceWidth(a.font, a.style);
    }
    if (text) {
      a.a = store(text, len);
      a.b = static_cast<uint32_t>(len);
    }
    word_.push_back(a);
  }

  uint32_t store(const char* s, size_t n) {
    const uint32_t at = static_cast<uint32_t>(text_.size());
    text_.append(s, n);
    text_.push_back('\0');
    return at;
  }

  // Characters the fonts do not draw and the layout has no use for.
  static bool invisible(uint32_t cp) {
    return cp == 0x00AD || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF;
  }

  void addText(const char* s, size_t n, uint8_t font, uint8_t style, uint16_t link) {
    if (!rtlKnown_) {
      rtlKnown_ = true;
      std::string probe(s, std::min<size_t>(n, 64));
      rtl_ = BidiUtils::detectParagraphLevel(probe.c_str(), 0) == 1;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
    const uint8_t* end = p + n;
    std::string piece;
    bool brkNext = false;
    uint32_t lastCp = 0;
    auto flush = [&]() {
      if (!piece.empty()) {
        Atom a;
        a.font = font;
        a.style = style;
        a.link = link;
        a.brk = brkNext;
        push(a, piece.data(), piece.size());
        piece.clear();
      }
      brkNext = false;
    };
    while (p < end) {
      const uint8_t* at = p;
      const uint32_t cp = xwa::nextUtf8(p, end);
      if (cp == ' ') {
        // A space: a line may break here.
        flush();
        pendingSpace_ = true;
        lastCp = cp;
        continue;
      }
      if (cp == 0x00A0 || cp == 0x2007 || cp == 0x202F) {
        // No-break spaces: a gap that keeps its neighbours on one line.
        flush();
        if (!pendingSpace_) pendingGlue_ = true;
        lastCp = cp;
        continue;
      }
      if (cp == 0x200B) {
        // Zero-width space: a break without a gap.
        flush();
        brkNext = true;
        lastCp = cp;
        continue;
      }
      if (invisible(cp)) continue;
      if ((isCjk(cp) || isCjk(lastCp)) && !piece.empty() && !noBreakBefore(cp) && !noBreakAfter(lastCp)) {
        // Between CJK characters a line may break, except around
        // punctuation that must stay with its neighbour.
        flush();
        brkNext = true;
      }
      piece.append(reinterpret_cast<const char*>(at), static_cast<size_t>(p - at));
      lastCp = cp;
    }
    flush();
  }

  void addMath(uint32_t pos, uint32_t len, uint8_t font, uint16_t link) {
    std::vector<uint8_t> buf;
    xwa::Formula f;
    const uint32_t back = r_.pos();
    const bool ok = xwa::loadFormula(r_, pos, len, buf, f);
    r_.seek(back);
    if (!ok) return;
    const xwa::GlyphTable* g = L_.session_.glyphs();
    const float ppu = st_.em[font] / (g ? g->upem() : 1000);
    Atom a;
    a.kind = Atom::Math;
    a.font = font;
    a.link = link;
    a.a = pos;
    a.b = len;
    a.w = static_cast<int16_t>(std::ceil(std::max<int32_t>(0, f.width) * ppu));
    a.up = static_cast<int16_t>(std::ceil(std::max<int32_t>(0, f.ascent) * ppu));
    a.down = static_cast<int16_t>(std::ceil(std::max<int32_t>(0, f.descent) * ppu));
    if (a.w <= 0) a.w = 1;
    push(a);
  }

  void addIcon(uint32_t image, uint8_t height16, uint8_t font, uint16_t link) {
    xwa::ImageRecord rec;
    if (!L_.session_.archive().imageRecord(image, rec) || rec.width == 0 || rec.height == 0) return;
    // Height in sixteenths of the body text size, whatever the text around.
    float h = st_.em[FONT_BODY] * (height16 ? height16 : 16) / 16.0f;
    h = std::min(h, static_cast<float>(L_.height_) / 3);
    float w = h * rec.width / rec.height;
    if (w > width_) {
      h = h * width_ / w;
      w = width_;
    }
    Atom a;
    a.kind = Atom::Icon;
    a.font = font;
    a.link = link;
    a.a = image;
    a.b = rec.kind;
    a.w = static_cast<int16_t>(std::max(1.0f, std::round(w)));
    const int16_t hh = static_cast<int16_t>(std::max(1.0f, std::round(h)));
    // Centred on the middle of lower-case letters, as browsers place
    // Wikipedia's icons.
    const int16_t mid = static_cast<int16_t>(st_.em[font] * 0.3f);
    a.up = static_cast<int16_t>(std::min<int>(hh, hh / 2 + mid));
    a.down = static_cast<int16_t>(hh - a.up);
    push(a);
  }

  int16_t wordWidth(size_t from = 0) const {
    int w = 0;
    for (size_t i = from; i < word_.size(); i++) w += word_[i].w + (i > from && word_[i].glue ? word_[i].spaceW : 0);
    return static_cast<int16_t>(w);
  }

  int16_t lineWidth() const {
    int w = 0;
    for (size_t i = 0; i < line_.size(); i++) {
      if (i > 0 && (line_[i].space || line_[i].glue)) w += line_[i].spaceW;
      w += line_[i].w;
    }
    return static_cast<int16_t>(w);
  }

  // Splits word_ so its first part (plus a hyphen when needed) fits in
  // `room`; the rest stays in word_. False when no break point fits.
  bool splitWord(int16_t room, bool anyBreak) {
    if (word_.size() != 1 || word_[0].kind != Atom::Text) {
      if (!anyBreak || word_.size() < 2) return false;
      // Several atoms (styles, boxes): break between them.
      int w = 0;
      size_t k = 0;
      while (k < word_.size() && w + word_[k].w <= room) w += word_[k++].w;
      if (k == 0 || k == word_.size()) return false;
      for (size_t i = 0; i < k; i++) line_.push_back(word_[i]);
      word_.erase(word_.begin(), word_.begin() + static_cast<long>(k));
      word_[0].space = false;
      word_[0].brk = false;
      return true;
    }
    const Atom whole = word_[0];
    const std::string text(text_.c_str() + whole.a, whole.b);
    std::vector<Hyphenator::BreakInfo> breaks;
    if (st_.hyphenate || anyBreak) breaks = Hyphenator::breakOffsets(text, anyBreak);
    if (anyBreak && breaks.empty()) {
      // Any code point boundary will do for a word wider than a line.
      const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
      const uint8_t* end = p + text.size();
      while (p < end) {
        xwa::nextUtf8(p, end);
        if (p < end) breaks.push_back({static_cast<size_t>(p - reinterpret_cast<const uint8_t*>(text.data())), false});
      }
    }
    // The longest first part that fits.
    for (auto it = breaks.rbegin(); it != breaks.rend(); ++it) {
      if (it->byteOffset == 0 || it->byteOffset >= text.size()) continue;
      std::string head = text.substr(0, it->byteOffset);
      if (it->requiresInsertedHyphen) head += '-';
      Atom first = whole;
      first.a = store(head.data(), head.size());
      first.b = static_cast<uint32_t>(head.size());
      first.w = measure(first);
      if (first.w > room) continue;
      Atom rest = whole;
      rest.space = false;
      rest.brk = false;
      rest.spaceW = 0;
      rest.a = whole.a + static_cast<uint32_t>(it->byteOffset);
      rest.b = whole.b - static_cast<uint32_t>(it->byteOffset);
      // Measuring needs the rest NUL-terminated on its own.
      rest.a = store(text_.c_str() + rest.a, rest.b);
      rest.w = measure(rest);
      line_.push_back(first);
      word_.clear();
      word_.push_back(rest);
      return true;
    }
    return false;
  }

  void commitWord() {
    if (word_.empty()) return;
    for (Atom& a : word_) {
      if (a.kind == Atom::Text) a.w = measure(a);
    }
    for (int guard = 0; guard < 1000 && !word_.empty(); guard++) {
      const int16_t gap = line_.empty() ? 0 : (word_[0].space ? word_[0].spaceW : 0);
      const int16_t used = lineWidth();
      const int16_t ww = wordWidth();
      if (used + gap + ww <= width_) {
        for (const Atom& a : word_) line_.push_back(a);
        word_.clear();
        return;
      }
      if (!line_.empty()) {
        // Hyphenate into the end of this line if the reader wants it.
        if (st_.hyphenate && splitWord(static_cast<int16_t>(width_ - used - gap), false)) {
          emitLine(false);
          continue;
        }
        emitLine(false);
        word_[0].space = false;
        continue;
      }
      // Alone on a line and still too wide.
      if (word_.size() == 1 && word_[0].kind != Atom::Text) {
        Atom& box = word_[0];
        const float k = static_cast<float>(width_) / box.w;
        box.w = width_;
        box.up = static_cast<int16_t>(box.up * k);
        box.down = static_cast<int16_t>(box.down * k);
        continue;
      }
      if (splitWord(width_, true)) {
        emitLine(false);
        continue;
      }
      // Nothing to break at: let it overflow rather than lose it.
      for (const Atom& a : word_) line_.push_back(a);
      word_.clear();
    }
  }

  void emitLine(bool last) {
    BlockLines& out = *out_;
    Line ln;
    ln.item = static_cast<uint16_t>(out.items.size());
    ln.area = static_cast<uint16_t>(out.areas.size());
    int16_t above = st_.ascent[lineFont_];
    int16_t below = static_cast<int16_t>(st_.lineHeight[lineFont_] - st_.ascent[lineFont_]);
    for (const Atom& a : line_) {
      if (a.kind == Atom::Text) {
        above = std::max(above, st_.ascent[a.font]);
        below = std::max<int16_t>(below, static_cast<int16_t>(st_.lineHeight[a.font] - st_.ascent[a.font]));
      } else {
        above = std::max(above, a.up);
        below = std::max(below, a.down);
      }
    }
    below = std::max<int16_t>(below, 0);
    ln.h = static_cast<int16_t>(above + below);
    ln.base = above;

    // Words: runs of atoms with no break opportunity between them.
    struct Word {
      size_t first, count;
      int16_t w, gap;
    };
    std::vector<Word> words;
    for (size_t i = 0; i < line_.size(); i++) {
      if (i == 0 || line_[i].space || line_[i].brk) {
        words.push_back({i, 0, 0, static_cast<int16_t>(i > 0 && line_[i].space ? line_[i].spaceW : 0)});
      }
      words.back().count++;
      words.back().w = static_cast<int16_t>(words.back().w + line_[i].w +
                                            (words.back().count > 1 && line_[i].glue ? line_[i].spaceW : 0));
    }
    int natural = 0;
    int gaps = 0;
    for (size_t i = 0; i < words.size(); i++) {
      natural += words[i].w + (i > 0 ? words[i].gap : 0);
      if (i > 0 && words[i].gap > 0) gaps++;
    }
    const int slack = std::max(0, width_ - natural);
    const bool justify = align_ == 0 && st_.justify && !last && gaps > 0;

    // Visual order: right-to-left paragraphs and mixed lines.
    std::vector<uint16_t> order;
    if (words.size() > 1) {
      std::vector<std::string> texts;
      texts.reserve(words.size());
      for (const Word& w : words) {
        std::string t;
        for (size_t k = 0; k < w.count; k++) {
          const Atom& a = line_[w.first + k];
          t += a.kind == Atom::Text ? text_.c_str() + a.a : "\xEF\xBF\xBC";  // object replacement
        }
        texts.push_back(std::move(t));
      }
      BidiUtils::computeVisualWordOrder(texts, rtl_, order);
    }
    if (order.empty()) {
      order.resize(words.size());
      for (size_t i = 0; i < words.size(); i++) order[i] = static_cast<uint16_t>(i);
    }

    int x = x_;
    if (!justify) {
      if (align_ == 2) {
        x += slack / 2;
      } else if (rtl_) {
        x += slack;
      }
    }
    int gapsLeft = gaps;
    int slackLeft = slack;
    for (size_t v = 0; v < order.size(); v++) {
      const Word& w = words[order[v]];
      if (v > 0) {
        // In visual order the gap belongs to whichever neighbour carries it
        // logically; spaces are equal, so take the larger.
        const Word& prev = words[order[v - 1]];
        const int16_t gap = std::max(w.gap, prev.gap);
        x += gap;
        if (justify && gap > 0 && gapsLeft > 0) {
          const int add = slackLeft / gapsLeft;
          x += add;
          slackLeft -= add;
          gapsLeft--;
        }
      }
      for (size_t k = 0; k < w.count; k++) {
        const Atom& a = line_[w.first + k];
        if (k > 0 && a.glue) x += a.spaceW;
        DrawItem it;
        it.x = static_cast<int16_t>(x);
        it.w = a.w;
        if (a.kind == Atom::Text) {
          it.kind = DrawItem::Text;
          it.font = a.font;
          it.style = static_cast<uint8_t>(a.style & ~EpdFontFamily::UNDERLINE);
          it.y = static_cast<int16_t>(above - st_.ascent[a.font]);
          // drawText halves SUP/SUB glyphs; moving them is the caller's job,
          // by the same amounts as the book reader.
          if (a.style & EpdFontFamily::SUP) {
            it.y = static_cast<int16_t>(it.y - st_.ascent[a.font] * 2 / 5);
          } else if (a.style & EpdFontFamily::SUB) {
            it.y = static_cast<int16_t>(it.y + st_.ascent[a.font] / 4);
          }
          it.h = st_.lineHeight[a.font];
          it.a = static_cast<uint32_t>(out.text.size());
          it.b = a.b;
          out.text.append(text_.c_str() + a.a, a.b);
          out.text.push_back('\0');
        } else {
          it.kind = a.kind == Atom::Math ? DrawItem::Formula : DrawItem::Image;
          it.font = a.font;
          it.y = static_cast<int16_t>(above - a.up);
          it.h = static_cast<int16_t>(a.up + a.down);
          it.a = a.a;
          it.b = a.b;
          if (a.kind == Atom::Icon) {
            it.flags = static_cast<uint8_t>(a.b);
            it.b = 0;
          }
        }
        out.items.push_back(it);
        if (a.link) {
          const xwa::InlineItem& target = links_[a.link - 1];
          if (a.kind == Atom::Text) {
            // Links are underlined so they can be seen on e-ink.
            DrawItem u;
            u.kind = DrawItem::Fill;
            u.x = static_cast<int16_t>(x);
            u.w = a.w;
            u.y = static_cast<int16_t>(above + std::max(1, st_.lineHeight[a.font] / 16));
            u.h = 1;
            out.items.push_back(u);
          }
          HitArea h;
          h.kind = HitArea::Link;
          h.anchored = target.anchored;
          h.x = static_cast<int16_t>(x);
          h.w = a.w;
          h.y = 0;
          h.h = ln.h;
          h.group = a.link;
          h.target = target.article;
          h.place = target.place;
          // Merge with the previous piece of the same link on this line.
          if (out.areas.size() > ln.area) {
            HitArea& p = out.areas.back();
            if (p.kind == HitArea::Link && p.group == h.group && p.x + p.w <= h.x && h.x - (p.x + p.w) < 40) {
              p.w = static_cast<int16_t>(h.x + h.w - p.x);
              x += a.w;
              continue;
            }
          }
          out.areas.push_back(h);
        }
        x += a.w;
      }
    }
    ln.items = static_cast<uint16_t>(out.items.size() - ln.item);
    ln.areas = static_cast<uint16_t>(out.areas.size() - ln.area);
    out.lines.push_back(ln);

    // Keep only the text of the word still being built.
    line_.clear();
    if (!word_.empty()) {
      std::string keep;
      for (Atom& a : word_) {
        if (a.kind != Atom::Text) continue;
        const uint32_t at = static_cast<uint32_t>(keep.size());
        keep.append(text_.c_str() + a.a, a.b);
        keep.push_back('\0');
        a.a = at;
      }
      text_.swap(keep);
    } else {
      text_.clear();
    }
  }

  Layout& L_;
  xwa::StreamReader& r_;
  GfxRenderer& rend_;
  const TextStyle& st_;
  BlockLines* out_ = nullptr;
  int16_t x_ = 0;
  int16_t width_ = 0;
  uint8_t align_ = 0;
  uint8_t lineFont_ = FONT_BODY;
  std::vector<Atom> line_;
  std::vector<Atom> word_;
  std::string text_;
  std::vector<xwa::InlineItem> links_;
  bool pendingSpace_ = false;
  bool pendingGlue_ = false;
  bool rtlKnown_ = false;
  bool rtl_ = false;
};

// ------------------------------------------------------------------ layout

Layout::Layout(GfxRenderer& renderer, WikiSession& session, uint32_t article, const xwa::ArticleHeader& header,
               const TextStyle& style, int16_t width, int16_t height)
    : renderer_(renderer), session_(session), header_(header), style_(style), width_(width), height_(height) {
  reader_.open(session.archive(), article);
}

Cursor Layout::first() const {
  Cursor c;
  c.block = header_.bodyStart;
  c.title = true;
  return c;
}

uint32_t Layout::bodyPos(const Cursor& c) const {
  if (c.end) return header_.bodySize;
  return c.block > header_.bodyStart ? std::min(c.block - header_.bodyStart, header_.bodySize) : 0;
}

int16_t Layout::indentOf(const Cursor& c) const {
  const int step = static_cast<int>(style_.em[FONT_BODY] * 1.4f);
  int levels = 0;
  for (int i = 0; i < std::min<int>(c.depth, Cursor::MAX_LEVELS); i++) {
    // A definition list's first level stays at the margin.
    if (!(c.levels[i].kind == 3 && i == 0)) levels++;
  }
  levels += std::max(0, c.depth - Cursor::MAX_LEVELS);
  return static_cast<int16_t>(std::min(levels * step, width_ * 2 / 5));
}

void Layout::applyStructure(Cursor& c, const xwa::BlockItem& b) const {
  switch (b.op) {
    case xwa::block_op::LIST_START:
    case xwa::block_op::QUOTE_START:
      if (c.depth < Cursor::MAX_LEVELS) {
        Cursor::Level& l = c.levels[c.depth];
        l = Cursor::Level();
        l.kind = b.op == xwa::block_op::QUOTE_START ? 0 : static_cast<uint8_t>(1 + std::min<uint8_t>(b.arg, 2));
      }
      if (c.depth < 255) c.depth++;
      break;
    case xwa::block_op::LIST_ITEM:
      if (c.depth > 0 && c.depth <= Cursor::MAX_LEVELS) {
        Cursor::Level& l = c.levels[c.depth - 1];
        l.number++;
        l.marker = l.kind == 1 || l.kind == 2;
      }
      break;
    case xwa::block_op::LIST_END:
    case xwa::block_op::QUOTE_END:
      if (c.depth > 0) c.depth--;
      break;
    default:
      break;
  }
}

bool Layout::flowRun(uint32_t pos, uint32_t len, uint8_t font, uint8_t style, int16_t width, uint8_t align,
                     BlockLines& out) {
  Flow f(*this, reader_);
  f.begin(0, width, align, &out);
  const bool ok = f.run(pos, len, font, style);
  f.finish();
  return ok;
}

void Layout::layPicture(uint32_t image, int16_t x, int16_t w, int16_t maxH, BlockLines& out) const {
  xwa::ImageRecord rec;
  if (!session_.archive().imageRecord(image, rec) || rec.width == 0 || rec.height == 0) return;
  // Shown at the size a browser would show it at this text size, within the
  // column and the height allowed.
  const float scale = style_.em[FONT_BODY] / 16.0f;
  float dw = rec.width * scale;
  float dh = rec.height * scale;
  if (dw > w) {
    dh = dh * w / dw;
    dw = w;
  }
  if (dh > maxH) {
    dw = dw * maxH / dh;
    dh = maxH;
  }
  Line ln;
  ln.item = static_cast<uint16_t>(out.items.size());
  ln.area = static_cast<uint16_t>(out.areas.size());
  DrawItem it;
  it.kind = DrawItem::Image;
  it.flags = rec.kind;
  it.w = static_cast<int16_t>(std::max(1.0f, std::round(dw)));
  it.h = static_cast<int16_t>(std::max(1.0f, std::round(dh)));
  it.x = static_cast<int16_t>(x + (w - it.w) / 2);
  it.a = image;
  out.items.push_back(it);
  HitArea a;
  a.kind = HitArea::Picture;
  a.x = it.x;
  a.w = it.w;
  a.h = it.h;
  a.target = image;
  out.areas.push_back(a);
  ln.items = 1;
  ln.areas = 1;
  ln.h = it.h;
  ln.base = it.h;
  out.lines.push_back(ln);
}

void Layout::layFormula(uint32_t pos, uint32_t len, int16_t x, int16_t w, BlockLines& out) {
  std::vector<uint8_t> buf;
  xwa::Formula f;
  if (!xwa::loadFormula(reader_, pos, len, buf, f)) return;
  const xwa::GlyphTable* g = session_.glyphs();
  float ppu = style_.em[FONT_BODY] / (g ? g->upem() : 1000);
  float fw = std::max<int32_t>(1, f.width) * ppu;
  if (fw > w) {
    ppu *= w / fw;
    fw = w;
  }
  const float fh = std::max<int32_t>(1, f.ascent + f.descent) * ppu;
  const int16_t pad = static_cast<int16_t>(style_.lineHeight[FONT_BODY] / 4);
  Line ln;
  ln.item = static_cast<uint16_t>(out.items.size());
  DrawItem it;
  it.kind = DrawItem::Formula;
  it.font = FONT_BODY;
  it.w = static_cast<int16_t>(std::ceil(fw));
  it.h = static_cast<int16_t>(std::ceil(fh));
  it.x = static_cast<int16_t>(x + (w - it.w) / 2);
  it.y = pad;
  it.a = pos;
  it.b = len;
  out.items.push_back(it);
  ln.items = 1;
  ln.h = static_cast<int16_t>(it.h + 2 * pad);
  ln.base = static_cast<int16_t>(pad + std::ceil(std::max<int32_t>(0, f.ascent) * ppu));
  out.lines.push_back(ln);
}

bool Layout::layPre(uint32_t pos, uint32_t len, int16_t x, int16_t w, BlockLines& out) {
  std::string text(len, '\0');
  if (len && (!reader_.seek(pos) || !reader_.bytes(&text[0], len))) return false;
  const int font = style_.id[FONT_SMALL];
  const int16_t lh = style_.lineHeight[FONT_SMALL];
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string rest = text.substr(start, end - start);
    // No wrapping in preformatted text, except where a line is wider than
    // the screen: then it is cut at the last character that fits.
    do {
      size_t fit = rest.size();
      if (renderer_.getTextAdvanceX(font, rest.c_str(), EpdFontFamily::REGULAR) > w) {
        // The longest prefix that fits, on a code point boundary.
        size_t lo = 0;
        size_t hi = rest.size();
        while (hi - lo > 1) {
          size_t mid = (lo + hi) / 2;
          while (mid > lo && (static_cast<uint8_t>(rest[mid]) & 0xC0) == 0x80) mid--;
          if (mid == lo) break;
          if (renderer_.getTextAdvanceX(font, rest.substr(0, mid).c_str(), EpdFontFamily::REGULAR) <= w) {
            lo = mid;
          } else {
            hi = mid;
          }
        }
        fit = lo;
      }
      if (fit == 0) {
        // Not even one character fits: take one anyway.
        fit = 1;
        while (fit < rest.size() && (static_cast<uint8_t>(rest[fit]) & 0xC0) == 0x80) fit++;
      }
      Line ln;
      ln.item = static_cast<uint16_t>(out.items.size());
      if (fit > 0) {
        DrawItem it;
        it.kind = DrawItem::Text;
        it.font = FONT_SMALL;
        it.x = x;
        it.h = lh;
        it.a = static_cast<uint32_t>(out.text.size());
        it.b = static_cast<uint32_t>(fit);
        out.text.append(rest, 0, fit);
        out.text.push_back('\0');
        out.items.push_back(it);
        ln.items = 1;
      }
      ln.h = lh;
      ln.base = style_.ascent[FONT_SMALL];
      out.lines.push_back(ln);
      rest.erase(0, fit);
    } while (!rest.empty());
    start = end + 1;
  }
  return true;
}

bool Layout::layTablePlaceholder(uint32_t index, int16_t x, int16_t w, BlockLines& out) {
  xwa::TableInfo t;
  if (!xwa::readTable(reader_, header_, index, t)) return true;  // left out rather than failing the page
  // A summary from the header row, so a reader can tell tables apart.
  std::string summary;
  for (const auto& c : t.cells) {
    if (c.row != 0) break;
    std::string cell = runText(reader_, c.runPos, c.runLen, 60);
    if (cell.empty()) continue;
    if (!summary.empty()) summary += " \xC2\xB7 ";  // middle dot
    summary += cell;
    if (summary.size() > 160) break;
  }
  const int16_t pad = 8;
  const int16_t innerW = static_cast<int16_t>(w - 2 * pad);
  BlockLines inner;
  Flow f(*this, reader_);
  char label[96];
  snprintf(label, sizeof(label), "%s %u \xC2\xB7 %u \xC3\x97 %u", tr(STR_WIKI_TABLE), static_cast<unsigned>(index + 1),
           static_cast<unsigned>(t.rows), static_cast<unsigned>(t.cols));
  f.begin(0, innerW, 1, &inner);
  f.text(label, FONT_SMALL, EpdFontFamily::BOLD);
  f.finish();
  if (!summary.empty()) {
    f.begin(0, innerW, 1, &inner);
    f.text(summary.c_str(), FONT_SMALL, EpdFontFamily::REGULAR);
    f.finish();
  }
  if (inner.lines.size() > 4) inner.lines.resize(4);  // the viewer shows the rest
  // One tall line: the frame, the text, and a tap area that opens the table.
  int16_t total = 2 * pad;
  for (const Line& l : inner.lines) total = static_cast<int16_t>(total + l.h);
  Line& ln = out.open(total);
  ln.base = static_cast<int16_t>(pad + (inner.lines.empty() ? 0 : inner.lines[0].base));
  DrawItem frame;
  frame.kind = DrawItem::Frame;
  frame.x = x;
  frame.w = w;
  frame.h = total;
  out.items.push_back(frame);
  int16_t y = pad;
  for (const Line& l : inner.lines) {
    out.copyItems(inner, l, static_cast<int16_t>(x + pad), y);
    y = static_cast<int16_t>(y + l.h);
  }
  HitArea area;
  area.kind = HitArea::Table;
  area.x = x;
  area.w = w;
  area.h = total;
  area.target = index;
  out.areas.push_back(area);
  out.close();
  return true;
}

bool Layout::layInfobox(int16_t x, int16_t w, BlockLines& out) {
  std::vector<xwa::InfoboxRow> rows;
  if (!xwa::readInfobox(reader_, header_, rows)) return true;
  const int16_t pad = 6;
  const int16_t inner = static_cast<int16_t>(w - 2 * pad);
  const int16_t labelW = static_cast<int16_t>(inner * 2 / 5);
  const int16_t valueX = static_cast<int16_t>(labelW + pad);
  const int16_t valueW = static_cast<int16_t>(inner - valueX);
  BlockLines body;
  Flow f(*this, reader_);
  auto spacer = [&](int16_t h, bool rule) {
    body.open(h);
    if (rule) {
      DrawItem r;
      r.kind = DrawItem::Fill;
      r.w = inner;
      r.h = 1;
      r.y = static_cast<int16_t>(h / 2);
      body.items.push_back(r);
    }
    body.close();
  };
  spacer(pad, false);
  bool title = true;
  for (const auto& row : rows) {
    BlockLines cols[2];
    switch (row.kind) {
      case 0:  // header: the first one is the infobox's title
        if (!title) spacer(5, true);
        f.begin(0, inner, 2, &cols[0]);
        if (!f.run(row.runPos, row.runLen, title ? FONT_BODY : FONT_SMALL, EpdFontFamily::BOLD)) return false;
        f.finish();
        body.append(cols[0], 0);
        title = false;
        break;
      case 1:  // image and caption
        layPicture(row.image, 0, inner, static_cast<int16_t>(height_ * 2 / 5), cols[0]);
        f.begin(0, inner, 2, &cols[0]);
        if (!f.run(row.runPos, row.runLen, FONT_SMALL, EpdFontFamily::REGULAR)) return false;
        f.finish();
        body.append(cols[0], 0);
        break;
      case 2: {  // label and value side by side
        f.begin(0, labelW, 1, &cols[0]);
        if (!f.run(row.runPos, row.runLen, FONT_SMALL, EpdFontFamily::BOLD)) return false;
        f.finish();
        f.begin(0, valueW, 1, &cols[1]);
        if (!f.run(row.run2Pos, row.run2Len, FONT_SMALL, EpdFontFamily::REGULAR)) return false;
        f.finish();
        const int16_t xs[2] = {0, valueX};
        zipColumns(cols, xs, 2, body);
        spacer(3, false);
        break;
      }
      default:  // full width
        f.begin(0, inner, 2, &cols[0]);
        if (!f.run(row.runPos, row.runLen, FONT_SMALL, EpdFontFamily::REGULAR)) return false;
        f.finish();
        body.append(cols[0], 0);
        break;
    }
  }
  spacer(pad, false);
  // The frame: side bars on every line, edges above the first and below the
  // last, so it stays whole when a page break falls inside the box.
  for (size_t i = 0; i < body.lines.size(); i++) {
    const Line& src = body.lines[i];
    Line& ln = out.open(src.h);
    ln.base = src.base;
    DrawItem bar;
    bar.kind = DrawItem::Fill;
    bar.w = 1;
    bar.h = src.h;
    bar.x = x;
    out.items.push_back(bar);
    bar.x = static_cast<int16_t>(x + w - 1);
    out.items.push_back(bar);
    if (i == 0 || i + 1 == body.lines.size()) {
      DrawItem edge;
      edge.kind = DrawItem::Fill;
      edge.x = x;
      edge.w = w;
      edge.h = 1;
      edge.y = static_cast<int16_t>(i == 0 ? 0 : src.h - 1);
      out.items.push_back(edge);
    }
    out.copyItems(body, src, static_cast<int16_t>(x + pad), 0);
    out.close();
  }
  return true;
}

bool Layout::layGallery(const xwa::BlockItem& b, int16_t x, int16_t w, BlockLines& out) {
  struct Item {
    uint32_t image, runPos, runLen;
  };
  std::vector<Item> items;
  uint32_t at = b.dataPos;
  for (uint32_t i = 0; i < b.value; i++) {
    Item it{};
    if (!reader_.seek(at) || !xwa::readGalleryItem(reader_, it.image, it.runPos, it.runLen)) return false;
    at = reader_.pos();
    items.push_back(it);
  }
  const int columns = (items.size() > 1 && w >= 300) ? 2 : 1;
  const int16_t gap = 10;
  const int16_t cellW = static_cast<int16_t>((w - gap * (columns - 1)) / columns);
  const int16_t xs[2] = {x, static_cast<int16_t>(x + cellW + gap)};
  Flow f(*this, reader_);
  for (size_t i = 0; i < items.size(); i += columns) {
    BlockLines cells[2];
    int n = 0;
    for (; n < columns && i + n < items.size(); n++) {
      const Item& it = items[i + n];
      layPicture(it.image, 0, cellW, static_cast<int16_t>(height_ / 3), cells[n]);
      f.begin(0, cellW, 2, &cells[n]);
      if (!f.run(it.runPos, it.runLen, FONT_SMALL, EpdFontFamily::REGULAR)) return false;
      f.finish();
    }
    const size_t first = out.lines.size();
    zipColumns(cells, xs, n, out);
    if (i > 0 && out.lines.size() > first) out.lines[first].gap = gap;
  }
  return true;
}

bool Layout::layBlock(const Cursor& at, const xwa::BlockItem& b, BlockLines& out) {
  const int16_t indent = indentOf(at);
  const int16_t x = indent;
  const int16_t w = static_cast<int16_t>(width_ - indent);
  const int16_t bodyLine = style_.lineHeight[FONT_BODY];
  const int16_t paraGap = static_cast<int16_t>(style_.extraParagraphSpacing ? bodyLine / 2 : bodyLine / 4);
  const bool inList = at.depth > 0 && at.depth <= Cursor::MAX_LEVELS && at.levels[at.depth - 1].kind != 0;
  const size_t first = out.lines.size();
  int16_t gap = inList ? static_cast<int16_t>(bodyLine / 6) : paraGap;
  Flow f(*this, reader_);
  switch (b.op) {
    case xwa::block_op::PARA:
      f.begin(x, w, 0, &out);
      if (!f.run(b.runPos, b.runLen, FONT_BODY, EpdFontFamily::REGULAR)) return false;
      f.finish();
      break;
    case xwa::block_op::HEADING: {
      const uint8_t level = b.arg;
      const uint8_t font = level <= 2 ? FONT_LARGE : FONT_BODY;
      const uint8_t style = level >= 5 ? (EpdFontFamily::BOLD | EpdFontFamily::ITALIC) : EpdFontFamily::BOLD;
      f.begin(x, w, 1, &out);
      if (!f.run(b.runPos, b.runLen, font, style)) return false;
      f.finish();
      if (level <= 2 && out.lines.size() > first) {
        // Wikipedia's rule under section headings.
        Line& last = out.lines.back();
        DrawItem rule;
        rule.kind = DrawItem::Fill;
        rule.x = x;
        rule.w = w;
        rule.h = 1;
        rule.y = static_cast<int16_t>(last.h + 2);
        out.items.push_back(rule);
        last.items = static_cast<uint16_t>(last.items + 1);
        last.h = static_cast<int16_t>(last.h + 5);
      }
      for (size_t i = first; i < out.lines.size(); i++) out.lines[i].keepWithNext = true;
      gap = static_cast<int16_t>(bodyLine * (level <= 2 ? 1.0f : 0.7f));
      break;
    }
    case xwa::block_op::PRE:
      if (!layPre(b.dataPos, b.dataLen, static_cast<int16_t>(x + 8), static_cast<int16_t>(w - 8), out)) return false;
      break;
    case xwa::block_op::RULE: {
      Line ln;
      ln.item = static_cast<uint16_t>(out.items.size());
      DrawItem rule;
      rule.kind = DrawItem::Fill;
      rule.x = static_cast<int16_t>(x + w / 6);
      rule.w = static_cast<int16_t>(w * 2 / 3);
      rule.h = 1;
      rule.y = static_cast<int16_t>(bodyLine / 2);
      out.items.push_back(rule);
      ln.items = 1;
      ln.h = bodyLine;
      out.lines.push_back(ln);
      break;
    }
    case xwa::block_op::FIGURE:
      layPicture(b.value, x, w, static_cast<int16_t>(height_ * 3 / 5), out);
      f.begin(static_cast<int16_t>(x + 8), static_cast<int16_t>(w - 16), 2, &out);
      if (!f.run(b.runPos, b.runLen, FONT_SMALL, EpdFontFamily::REGULAR)) return false;
      f.finish();
      // Keep a picture with the first line of its caption.
      if (out.lines.size() > first + 1) out.lines[first].keepWithNext = true;
      break;
    case xwa::block_op::MATH_BLOCK:
      layFormula(b.dataPos, b.dataLen, x, w, out);
      break;
    case xwa::block_op::TABLE_REF:
      if (!layTablePlaceholder(b.value, x, w, out)) return false;
      break;
    case xwa::block_op::INFOBOX_REF:
      if (!layInfobox(x, w, out)) return false;
      break;
    case xwa::block_op::GALLERY:
      if (!layGallery(b, x, w, out)) return false;
      break;
    default:
      break;
  }
  if (out.lines.size() > first) out.lines[first].gap = gap;
  return true;
}

void Layout::place(const BlockLines& lines, uint16_t index, int16_t y, const Cursor& c, Page& out,
                   uint32_t& groupBase) {
  const Line& ln = lines.lines[index];
  for (uint16_t k = 0; k < ln.items; k++) {
    DrawItem it = lines.items[ln.item + k];
    it.y = static_cast<int16_t>(it.y + y);
    if (it.kind == DrawItem::Text) {
      const uint32_t from = it.a;
      it.a = static_cast<uint32_t>(out.text.size());
      out.text.append(lines.text.c_str() + from, it.b);
      out.text.push_back('\0');
    }
    out.items.push_back(it);
  }
  for (uint16_t k = 0; k < ln.areas; k++) {
    HitArea a = lines.areas[ln.area + k];
    a.y = static_cast<int16_t>(a.y + y);
    if (a.kind == HitArea::Link) a.group = static_cast<uint16_t>(a.group + groupBase);
    out.areas.push_back(a);
  }
  const int step = static_cast<int>(style_.em[FONT_BODY] * 1.4f);
  // List marker, on the first line of an item's first block.
  if (index == 0 && c.depth > 0 && c.depth <= Cursor::MAX_LEVELS) {
    const Cursor::Level& l = c.levels[c.depth - 1];
    if (l.marker) {
      char marker[12];
      if (l.kind == 2) {
        snprintf(marker, sizeof(marker), "%u.", static_cast<unsigned>(l.number));
      } else {
        // A bullet, then a dash for the next level down.
        int bullets = 0;
        for (int i = 0; i < c.depth; i++) bullets += c.levels[i].kind == 1;
        std::strcpy(marker, bullets % 2 == 0 ? "\xE2\x80\x93" : "\xE2\x80\xA2");
      }
      const int16_t indent = indentOf(c);
      const int mw = renderer_.getTextAdvanceX(style_.id[FONT_BODY], marker, EpdFontFamily::REGULAR);
      DrawItem it;
      it.kind = DrawItem::Text;
      it.font = FONT_BODY;
      it.x = static_cast<int16_t>(std::max(0, indent - mw - step / 4));
      it.y = static_cast<int16_t>(y + ln.base - style_.ascent[FONT_BODY]);
      it.w = static_cast<int16_t>(mw);
      it.h = style_.lineHeight[FONT_BODY];
      it.a = static_cast<uint32_t>(out.text.size());
      it.b = static_cast<uint32_t>(std::strlen(marker));
      out.text += marker;
      out.text.push_back('\0');
      out.items.push_back(it);
    }
  }
  // A bar beside quoted lines.
  int levels = 0;
  for (int i = 0; i < std::min<int>(c.depth, Cursor::MAX_LEVELS); i++) {
    if (!(c.levels[i].kind == 3 && i == 0)) levels++;
    if (c.levels[i].kind != 0) continue;
    DrawItem bar;
    bar.kind = DrawItem::Fill;
    bar.x = static_cast<int16_t>(std::max(0, levels * step - step / 2 - 1));
    bar.y = y;
    bar.w = 2;
    bar.h = ln.h;
    out.items.push_back(bar);
  }
}

bool Layout::page(const Cursor& at, Page& out, Cursor& next) {
  out.clear();
  out.start = at;
  Cursor c = at;
  out.startPos = bodyPos(c);
  int16_t y = 0;
  uint32_t groupBase = 0;
  if (c.title) {
    // The article's title opens its first page, as a heading with a rule.
    BlockLines t;
    Flow f(*this, reader_);
    f.begin(0, width_, 1, &t);
    f.text(header_.title.c_str(), FONT_LARGE, EpdFontFamily::BOLD);
    f.finish();
    if (!t.lines.empty()) {
      Line& last = t.lines.back();
      DrawItem rule;
      rule.kind = DrawItem::Fill;
      rule.w = width_;
      rule.h = 1;
      rule.y = static_cast<int16_t>(last.h + 3);
      t.items.push_back(rule);
      last.items = static_cast<uint16_t>(last.items + 1);
      last.h = static_cast<int16_t>(last.h + 6);
    }
    for (uint16_t i = 0; i < t.lines.size(); i++) {
      place(t, i, y, c, out, groupBase);
      y = static_cast<int16_t>(y + t.lines[i].h);
    }
    y = static_cast<int16_t>(y + style_.lineHeight[FONT_BODY] / 2);
    c.title = false;
  }
  for (int guard = 0; guard < 100000; guard++) {
    if (c.end || c.block >= header_.bodyEnd()) {
      c.end = true;
      break;
    }
    xwa::BlockItem b;
    if (!reader_.seek(c.block) || !xwa::readBlock(reader_, header_.bodyEnd(), b)) return false;
    if (b.op == 0) {
      c.end = true;
      break;
    }
    switch (b.op) {
      case xwa::block_op::LIST_START:
      case xwa::block_op::LIST_ITEM:
      case xwa::block_op::LIST_END:
      case xwa::block_op::QUOTE_START:
      case xwa::block_op::QUOTE_END:
        applyStructure(c, b);
        c.block = b.next;
        c.line = 0;
        continue;
      default:
        break;
    }
    const int16_t indent = indentOf(c);
    if (cacheBlock_ != c.block || cacheIndent_ != indent) {
      cache_.clear();
      cacheBlock_ = UINT32_MAX;
      if (!layBlock(c, b, cache_)) return false;
      cacheBlock_ = c.block;
      cacheIndent_ = indent;
    }
    for (uint16_t i = c.line; i < cache_.lines.size(); i++) {
      const Line& ln = cache_.lines[i];
      const int16_t gap = y > 0 ? ln.gap : 0;
      int need = gap + ln.h;
      if (ln.keepWithNext) {
        // Leave room for what follows a heading or picture.
        need += i + 1 < cache_.lines.size() ? cache_.lines[i + 1].h : style_.lineHeight[FONT_BODY];
      }
      if (y > 0 && y + need > height_) {
        c.line = i;
        next = c;
        out.endPos = bodyPos(c);
        return true;
      }
      place(cache_, i, static_cast<int16_t>(y + gap), c, out, groupBase);
      y = static_cast<int16_t>(y + gap + ln.h);
      if (i == 0 && c.depth > 0 && c.depth <= Cursor::MAX_LEVELS) c.levels[c.depth - 1].marker = 0;
    }
    // Links of the next block are told apart from this one's.
    groupBase += 256;
    c.block = b.next;
    c.line = 0;
  }
  next = c;
  next.end = true;
  out.endPos = header_.bodySize;
  return true;
}

}  // namespace wiki
