#include "XwaContent.h"

#include <algorithm>

namespace xwa {

// ------------------------------------------------------------------ stream

bool StreamReader::open(Archive& archive, uint32_t articleId) {
  ar_ = &archive;
  data_ = nullptr;
  dataChunk_ = UINT32_MAX;
  ArticleRecord rec;
  if (!archive.articleRecord(articleId, rec) || rec.chunkCount == 0) return false;
  firstChunk_ = rec.firstChunk;
  offset0_ = rec.offsetInChunk;
  const uint64_t room = (static_cast<uint64_t>(archive.chunkCount() - firstChunk_) << archive.chunkBits()) - offset0_;
  size_ = static_cast<uint32_t>(std::min<uint64_t>(room, 0xFFFFFFFFu));
  pos_ = 0;
  uint64_t streamSize = 0;
  if (!varint(streamSize)) return false;
  const uint64_t total = pos_ + streamSize;
  if (total > room) return false;
  // The stream must end in the record's last chunk.
  const uint64_t lastChunk = firstChunk_ + ((offset0_ + total - 1) >> archive.chunkBits());
  if (lastChunk != static_cast<uint64_t>(firstChunk_) + rec.chunkCount - 1) return false;
  size_ = static_cast<uint32_t>(total);
  return true;
}

bool StreamReader::load() {
  const uint32_t bits = ar_->chunkBits();
  const uint32_t chunk = firstChunk_ + ((offset0_ + pos_) >> bits);
  if (data_ && chunk == dataChunk_ && epoch_ == ar_->chunkEpoch()) return true;
  data_ = ar_->chunk(chunk, dataLen_);
  epoch_ = ar_->chunkEpoch();
  dataChunk_ = data_ ? chunk : UINT32_MAX;
  return data_ != nullptr;
}

bool StreamReader::seek(uint32_t pos) {
  if (pos > size_) return false;
  pos_ = pos;
  return true;
}

bool StreamReader::u8(uint8_t& v) {
  if (pos_ >= size_ || !load()) return false;
  const uint32_t off = (offset0_ + pos_) & (ar_->chunkSize() - 1);
  if (off >= dataLen_) return false;
  v = data_[off];
  pos_++;
  return true;
}

bool StreamReader::bytes(void* dst, size_t len) {
  auto* out = static_cast<uint8_t*>(dst);
  if (len > size_ - pos_) return false;
  while (len > 0) {
    if (!load()) return false;
    const uint32_t off = (offset0_ + pos_) & (ar_->chunkSize() - 1);
    if (off >= dataLen_) return false;
    const size_t n = std::min<size_t>(len, dataLen_ - off);
    std::memcpy(out, data_ + off, n);
    out += n;
    pos_ += static_cast<uint32_t>(n);
    len -= n;
  }
  return true;
}

bool StreamReader::skip(uint32_t len) {
  if (len > size_ - pos_) return false;
  pos_ += len;
  return true;
}

bool StreamReader::u16(uint16_t& v) {
  uint8_t b[2];
  if (!bytes(b, 2)) return false;
  v = le16(b);
  return true;
}

bool StreamReader::u32(uint32_t& v) {
  uint8_t b[4];
  if (!bytes(b, 4)) return false;
  v = le32(b);
  return true;
}

bool StreamReader::varint(uint64_t& v) {
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    uint8_t b = 0;
    if (!u8(b)) return false;
    v |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;
}

bool StreamReader::varint32(uint32_t& v) {
  uint64_t x = 0;
  if (!varint(x) || x > 0xFFFFFFFFull) return false;
  v = static_cast<uint32_t>(x);
  return true;
}

bool StreamReader::svarint32(int32_t& v) {
  uint64_t x = 0;
  if (!varint(x)) return false;
  const int64_t s = unzigzag(x);
  if (s < INT32_MIN || s > INT32_MAX) return false;
  v = static_cast<int32_t>(s);
  return true;
}

bool StreamReader::string(std::string& out, uint32_t maxLen) {
  uint32_t len = 0;
  if (!varint32(len) || len > maxLen) return false;
  out.resize(len);
  return len == 0 || bytes(&out[0], len);
}

// ------------------------------------------------------------------ header

bool readHeader(StreamReader& r, ArticleHeader& h) {
  h = ArticleHeader();
  uint64_t streamSize = 0;
  uint32_t count = 0;
  if (!r.seek(0) || !r.varint(streamSize) || !r.string(h.title, 1024) || !r.varint32(h.bodySize)) return false;
  if (!r.varint32(count) || count > 20000) return false;
  h.toc.resize(count);
  for (auto& e : h.toc) {
    if (!r.u8(e.level) || !r.string(e.text, 4096) || !r.varint32(e.pos)) return false;
  }
  if (!r.varint32(count) || count > 20000) return false;
  h.tables.resize(count);
  for (auto& t : h.tables) {
    if (!r.varint32(t)) return false;
  }
  uint32_t infobox = 0;
  if (!r.varint32(infobox)) return false;
  h.hasInfobox = infobox != 0;
  h.infobox = infobox ? infobox - 1 : 0;
  if (!r.varint32(count) || count > 20000) return false;
  h.anchors.resize(count);
  for (auto& a : h.anchors) {
    if (!r.u32(a.hash) || !r.varint32(a.section)) return false;
  }
  h.bodyStart = r.pos();
  const uint32_t room = r.size() - h.bodyStart;
  if (h.bodySize > room) return false;
  for (const auto& e : h.toc) {
    if (e.pos > h.bodySize) return false;
  }
  for (const uint32_t t : h.tables) {
    if (t >= room) return false;
  }
  return !h.hasInfobox || h.infobox < room;
}

uint32_t anchorHash(const char* name, size_t len) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < len; i++) {
    const uint8_t b = name[i] == ' ' ? '_' : static_cast<uint8_t>(name[i]);
    h = (h ^ b) * 0x01000193u;
  }
  return h;
}

uint32_t headingAnchorHash(const std::string& text) {
  uint32_t h = 0x811C9DC5u;
  bool started = false;
  bool pendingSep = false;
  for (const char c : text) {
    if (c == ' ') {
      pendingSep = started;
      continue;
    }
    if (pendingSep) h = (h ^ static_cast<uint8_t>('_')) * 0x01000193u;
    pendingSep = false;
    started = true;
    h = (h ^ static_cast<uint8_t>(c)) * 0x01000193u;
  }
  return h;
}

int findPlace(const ArticleHeader& h, uint32_t hash) {
  for (const auto& a : h.anchors) {
    if (a.hash == hash) return static_cast<int>(a.section);
  }
  for (size_t i = 0; i < h.toc.size(); i++) {
    if (headingAnchorHash(h.toc[i].text) == hash) return static_cast<int>(i + 1);
  }
  return -1;
}

// ------------------------------------------------------------------ blocks

namespace {
bool run(StreamReader& r, uint32_t& pos, uint32_t& len) {
  if (!r.varint32(len)) return false;
  pos = r.pos();
  return r.skip(len);
}
}  // namespace

bool readBlock(StreamReader& r, uint32_t end, BlockItem& out) {
  for (;;) {
    out = BlockItem();
    out.start = r.pos();
    if (r.pos() >= end) return r.pos() == end;
    uint8_t op = 0;
    if (!r.u8(op)) return false;
    out.op = op;
    bool ok = true;
    switch (op) {
      case block_op::PARA:
        ok = run(r, out.runPos, out.runLen);
        break;
      case block_op::HEADING:
        ok = r.u8(out.arg) && run(r, out.runPos, out.runLen);
        break;
      case block_op::LIST_START:
        ok = r.u8(out.arg);
        break;
      case block_op::LIST_ITEM:
      case block_op::LIST_END:
      case block_op::QUOTE_START:
      case block_op::QUOTE_END:
      case block_op::RULE:
      case block_op::INFOBOX_REF:
        break;
      case block_op::PRE:
      case block_op::MATH_BLOCK:
        ok = run(r, out.dataPos, out.dataLen);
        break;
      case block_op::FIGURE:
        ok = r.varint32(out.value) && run(r, out.runPos, out.runLen);
        break;
      case block_op::TABLE_REF:
        ok = r.varint32(out.value);
        break;
      case block_op::GALLERY: {
        ok = r.varint32(out.value) && out.value < 10000;
        out.dataPos = r.pos();
        for (uint32_t i = 0; ok && i < out.value; i++) {
          uint32_t image = 0;
          uint32_t pos = 0;
          uint32_t len = 0;
          ok = r.varint32(image) && run(r, pos, len);
        }
        out.dataLen = r.pos() - out.dataPos;
        break;
      }
      case block_op::EXT: {
        uint32_t len = 0;
        if (!r.varint32(len) || !r.skip(len)) return false;
        continue;  // skippable: read the next block
      }
      default:
        return false;
    }
    if (!ok || r.pos() > end) return false;
    out.next = r.pos();
    return true;
  }
}

bool readInline(StreamReader& r, uint32_t runEnd, InlineItem& out, bool& damaged) {
  damaged = false;
  for (;;) {
    if (r.pos() >= runEnd) {
      damaged = r.pos() != runEnd;
      return false;
    }
    uint8_t op = 0;
    if (!r.u8(op)) {
      damaged = true;
      return false;
    }
    out = InlineItem();
    bool ok = true;
    if (op >= 0x01 && op <= 0x7F) {
      out.kind = InlineItem::Text;
      out.pos = r.pos();
      out.len = op;
      ok = r.skip(op);
    } else {
      switch (op) {
        case inline_op::TEXT_LONG:
          out.kind = InlineItem::Text;
          ok = run(r, out.pos, out.len);
          break;
        case inline_op::STYLE_PUSH:
          out.kind = InlineItem::StylePush;
          ok = r.u8(out.style);
          break;
        case inline_op::STYLE_POP:
          out.kind = InlineItem::StylePop;
          break;
        case inline_op::LINK_START:
          out.kind = InlineItem::Link;
          ok = r.varint32(out.article) && r.varint32(out.place);
          break;
        case inline_op::LINK_ANCHOR:
          out.kind = InlineItem::Link;
          out.anchored = true;
          ok = r.varint32(out.article) && r.u32(out.place);
          break;
        case inline_op::LINK_END:
          out.kind = InlineItem::LinkEnd;
          break;
        case inline_op::MATH:
          out.kind = InlineItem::Math;
          ok = run(r, out.pos, out.len);
          break;
        case inline_op::ICON:
          out.kind = InlineItem::Icon;
          ok = r.varint32(out.value) && r.u8(out.height);
          break;
        case inline_op::BREAK:
          out.kind = InlineItem::Break;
          break;
        case inline_op::NOTE_REF:
          out.kind = InlineItem::NoteRef;
          ok = r.varint32(out.value);
          break;
        case inline_op::EXT: {
          uint32_t pos = 0;
          uint32_t len = 0;
          if (!run(r, pos, len)) ok = false;
          if (ok && r.pos() <= runEnd) continue;
          break;
        }
        default:
          ok = false;
      }
    }
    if (!ok || r.pos() > runEnd) {
      damaged = true;
      return false;
    }
    return true;
  }
}

bool readGalleryItem(StreamReader& r, uint32_t& image, uint32_t& runPos, uint32_t& runLen) {
  return r.varint32(image) && run(r, runPos, runLen);
}

bool readTable(StreamReader& r, const ArticleHeader& h, uint32_t index, TableInfo& out, uint32_t maxCells) {
  out = TableInfo();
  if (index >= h.tables.size() || !r.seek(h.at(h.tables[index]))) return false;
  uint32_t count = 0;
  if (!r.varint32(out.rows) || !r.varint32(out.cols) || !r.u8(out.flags) || !r.varint32(count)) return false;
  if (count > maxCells || out.rows > 0xFFFF || out.cols > 0xFFFF) return false;
  const uint64_t slots = static_cast<uint64_t>(out.rows) * out.cols;
  if (slots > 262144) return false;
  std::vector<bool> taken(static_cast<size_t>(slots), false);
  out.cells.resize(count);
  uint32_t cursor = 0;  // row-major slot of the next free search
  for (auto& c : out.cells) {
    uint8_t flags = 0;
    uint32_t rs = 0;
    uint32_t cs = 0;
    if (!r.u8(flags) || !r.varint32(rs) || !r.varint32(cs) || !run(r, c.runPos, c.runLen)) return false;
    c.header = flags & 1;
    while (cursor < slots && taken[cursor]) cursor++;
    if (cursor >= slots) {
      // More cells than the grid holds: keep them in an extra row.
      c.row = static_cast<uint16_t>(out.rows ? out.rows - 1 : 0);
      c.col = 0;
      continue;
    }
    c.row = static_cast<uint16_t>(cursor / out.cols);
    c.col = static_cast<uint16_t>(cursor % out.cols);
    c.rowspan = static_cast<uint16_t>(std::max<uint32_t>(1, std::min<uint32_t>(rs, out.rows - c.row)));
    c.colspan = static_cast<uint16_t>(std::max<uint32_t>(1, std::min<uint32_t>(cs, out.cols - c.col)));
    for (uint32_t y = c.row; y < static_cast<uint32_t>(c.row) + c.rowspan; y++) {
      for (uint32_t x = c.col; x < static_cast<uint32_t>(c.col) + c.colspan; x++) {
        taken[static_cast<size_t>(y) * out.cols + x] = true;
      }
    }
  }
  return true;
}

bool readInfobox(StreamReader& r, const ArticleHeader& h, std::vector<InfoboxRow>& out) {
  out.clear();
  if (!h.hasInfobox || !r.seek(h.at(h.infobox))) return false;
  uint32_t count = 0;
  if (!r.varint32(count) || count > 4096) return false;
  out.resize(count);
  for (auto& row : out) {
    if (!r.u8(row.kind)) return false;
    bool ok = true;
    switch (row.kind) {
      case 0:
      case 3:
        ok = run(r, row.runPos, row.runLen);
        break;
      case 1:
        ok = r.varint32(row.image) && run(r, row.runPos, row.runLen);
        break;
      case 2:
        ok = run(r, row.runPos, row.runLen) && run(r, row.run2Pos, row.run2Len);
        break;
      default:
        ok = false;
    }
    if (!ok) return false;
  }
  return true;
}

// ------------------------------------------------------------------ math

bool parseFormula(const uint8_t* data, size_t len, Formula& out) {
  out = Formula();
  const uint8_t* p = data;
  const uint8_t* end = data + len;
  if (p >= end) return false;
  const uint8_t flags = *p++;
  out.display = flags & 1;
  out.hasFallback = flags & 2;
  uint64_t v = 0;
  int32_t* fields[3] = {&out.width, &out.ascent, &out.descent};
  for (int32_t* f : fields) {
    if (!getVarint(p, end, v)) return false;
    *f = static_cast<int32_t>(unzigzag(v));
  }
  if (out.hasFallback && !getVarint32(p, end, out.fallbackImage)) return false;
  out.items = p;
  out.end = end;
  return true;
}

bool loadFormula(StreamReader& r, uint32_t pos, uint32_t len, std::vector<uint8_t>& buf, Formula& out) {
  if (len > 1u << 20 || !r.seek(pos)) return false;
  buf.resize(len);
  if (len && !r.bytes(buf.data(), len)) return false;
  return parseFormula(buf.data(), buf.size(), out);
}

namespace {
bool svar(const uint8_t*& p, const uint8_t* end, int32_t& out) {
  uint64_t v = 0;
  if (!getVarint(p, end, v)) return false;
  out = static_cast<int32_t>(unzigzag(v));
  return true;
}
}  // namespace

bool MathItems::next(MathItem& out) {
  if (p_ >= end_ || unknown_) return false;
  out = MathItem();
  const uint8_t tag = *p_++;
  int32_t dx = 0;
  int32_t dy = 0;
  bool ok = true;
  switch (tag) {
    case 0:
      out.kind = MathItem::Glyph;
      ok = getVarint32(p_, end_, out.glyph) && p_ < end_;
      if (ok) out.scale = *p_++;
      ok = ok && svar(p_, end_, dx) && svar(p_, end_, dy);
      break;
    case 1:
      out.kind = MathItem::GlyphXY;
      ok = getVarint32(p_, end_, out.glyph) && end_ - p_ >= 4;
      if (ok) {
        out.sx = static_cast<int16_t>(le16(p_));
        out.sy = static_cast<int16_t>(le16(p_ + 2));
        p_ += 4;
      }
      ok = ok && svar(p_, end_, dx) && svar(p_, end_, dy);
      break;
    case 2:
      out.kind = MathItem::Rule;
      ok = svar(p_, end_, dx) && svar(p_, end_, dy) && getVarint32(p_, end_, out.w) && getVarint32(p_, end_, out.h);
      break;
    case 3: {
      out.kind = MathItem::Path;
      ok = svar(p_, end_, dx) && svar(p_, end_, dy);
      out.path = p_;
      uint32_t count = 0;
      ok = ok && getVarint32(p_, end_, count);
      static const uint8_t points[5] = {1, 1, 2, 3, 0};
      for (uint32_t i = 0; ok && i < count; i++) {
        if (p_ >= end_ || *p_ > 4) {
          ok = false;
          break;
        }
        const uint8_t n = points[*p_++];
        for (uint8_t k = 0; ok && k < 2 * n; k++) {
          uint64_t v = 0;
          ok = getVarint(p_, end_, v);
        }
      }
      out.pathEnd = p_;
      break;
    }
    default:
      unknown_ = true;
      return false;
  }
  if (!ok) {
    unknown_ = true;
    return false;
  }
  x_ += dx;
  y_ += dy;
  out.x = x_;
  out.y = y_;
  return true;
}

PathCommands::PathCommands(const MathItem& item) : p_(item.path), end_(item.pathEnd) {
  if (!p_ || !getVarint32(p_, end_, remaining_)) {
    remaining_ = 0;
    bad_ = p_ != nullptr;
  }
}

bool PathCommands::next(PathCommand& out) {
  if (remaining_ == 0 || bad_) return false;
  remaining_--;
  if (p_ >= end_ || *p_ > 4) {
    bad_ = true;
    return false;
  }
  out = PathCommand();
  out.op = *p_++;
  static const uint8_t points[5] = {1, 1, 2, 3, 0};
  for (uint8_t k = 0; k < points[out.op]; k++) {
    int32_t dx = 0;
    int32_t dy = 0;
    if (!svar(p_, end_, dx) || !svar(p_, end_, dy)) {
      bad_ = true;
      return false;
    }
    penX_ += dx;
    penY_ += dy;
    out.pt[2 * k] = penX_;
    out.pt[2 * k + 1] = penY_;
  }
  return true;
}

}  // namespace xwa
