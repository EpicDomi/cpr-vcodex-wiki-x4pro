#include "XwaSearch.h"

#include <algorithm>

#include "XwaCrc.h"

namespace xwa {

namespace {
constexpr size_t PAYLOAD_END = BLOCK_SIZE - 4;
constexpr size_t LEAF_HEADER = 4;
constexpr size_t INNER_HEADER = 6;

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Front-coded string: varint shared, varint suffix length, suffix bytes.
bool frontCoded(const uint8_t*& p, const uint8_t* end, std::string& prev) {
  uint32_t shared = 0;
  uint32_t len = 0;
  if (!getVarint32(p, end, shared) || !getVarint32(p, end, len)) return false;
  if (shared > prev.size() || len > static_cast<size_t>(end - p)) return false;
  prev.resize(shared);
  prev.append(reinterpret_cast<const char*>(p), len);
  p += len;
  return true;
}
}  // namespace

bool TitleIndex::fold(const char* utf8, std::string& key) {
  key.clear();
  std::string folded;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8);
  const uint8_t* end = p + std::strlen(utf8);
  while (p < end) {
    const uint32_t cp = nextUtf8(p, end);
    if (cp < 0x80) {
      char c = static_cast<char>(cp);
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c + ('a' - 'A'));
      } else if (c == '_' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') {
        c = ' ';
      }
      folded.push_back(c);
    } else if (cp >= 0xAC00 && cp <= 0xD7A3) {
      const uint32_t s = cp - 0xAC00;
      appendUtf8(folded, 0x1100 + s / 588);
      appendUtf8(folded, 0x1161 + (s % 588) / 28);
      if (s % 28) appendUtf8(folded, 0x11A7 + s % 28);
    } else {
      bool found = false;
      if (!ar_.foldLookup(cp, folded, found)) return false;
      if (!found) appendUtf8(folded, cp);
    }
  }
  bool space = false;
  for (const char c : folded) {
    if (c == ' ') {
      space = !key.empty();
      continue;
    }
    if (space) key.push_back(' ');
    space = false;
    key.push_back(c);
  }
  return true;
}

uint32_t TitleIndex::leafCount() const {
  return static_cast<uint32_t>(ar_.sectionInfo(section::TINDEX_LEAF).length / BLOCK_SIZE);
}

uint32_t TitleIndex::innerCount() const {
  return static_cast<uint32_t>(ar_.sectionInfo(section::TINDEX_INNER).length / BLOCK_SIZE);
}

const uint8_t* TitleIndex::block(bool inner, uint32_t index) {
  const SectionInfo& s = ar_.sectionInfo(inner ? section::TINDEX_INNER : section::TINDEX_LEAF);
  if (index >= s.length / BLOCK_SIZE) return nullptr;
  bool* checked = nullptr;
  const uint8_t* b = ar_.cachedBlock(s.offset + static_cast<uint64_t>(index) * BLOCK_SIZE, &checked);
  if (!b) return nullptr;
  blockReads_++;
  if (!*checked) {
    if (crc32(0, b, PAYLOAD_END) != le32(b + PAYLOAD_END)) return nullptr;
    *checked = true;
  }
  return b;
}

bool TitleIndex::Leaf::begin(const uint8_t* block) {
  const uint16_t used = le16(block + 2);
  if (LEAF_HEADER + used > PAYLOAD_END) {
    bad_ = true;
    return false;
  }
  remaining_ = le16(block);
  p_ = block + LEAF_HEADER;
  end_ = p_ + used;
  bad_ = false;
  return true;
}

bool TitleIndex::Leaf::next(Entry& e) {
  if (remaining_ == 0 || bad_) return false;
  remaining_--;
  uint32_t article = 0;
  if (!frontCoded(p_, end_, e.key) || !frontCoded(p_, end_, e.hit.display) || !getVarint32(p_, end_, article) ||
      end_ - p_ < 2) {
    bad_ = true;
    return false;
  }
  e.hit.article = article;
  e.hit.flags = *p_++;
  e.hit.popularity = *p_++;
  e.hit.anchor = 0;
  if (e.hit.flags & entry_flag::ANCHOR) {
    if (end_ - p_ < 4) {
      bad_ = true;
      return false;
    }
    e.hit.anchor = le32(p_);
    p_ += 4;
  }
  return true;
}

bool TitleIndex::children(const uint8_t* b, uint8_t& level, std::vector<Child>& out) {
  out.clear();
  level = b[0];
  const uint16_t count = le16(b + 2);
  const uint16_t used = le16(b + 4);
  if (level == 0 || INNER_HEADER + used > PAYLOAD_END) return false;
  const uint8_t* p = b + INNER_HEADER;
  const uint8_t* end = p + used;
  std::string sep;
  out.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    if (!frontCoded(p, end, sep) || end - p < 5) return false;
    out.push_back(Child{sep, le32(p), p[4]});
    p += 5;
  }
  return !out.empty();
}

bool TitleIndex::descend(const std::string& key, uint32_t& leaf) {
  leaf = 0;
  if (innerCount() == 0) return true;
  uint32_t index = 0;
  std::vector<Child> kids;
  for (int depth = 0; depth < 32; depth++) {
    const uint8_t* b = block(true, index);
    uint8_t level = 0;
    if (!b || !children(b, level, kids)) return false;
    // The last child whose separator is less than the key, else the first.
    size_t chosen = 0;
    for (size_t i = 0; i < kids.size(); i++) {
      if (kids[i].sep < key)
        chosen = i;
      else
        break;
    }
    if (level == 1) {
      leaf = kids[chosen].child;
      return leaf < leafCount();
    }
    index = kids[chosen].child;
    if (index >= innerCount()) return false;
  }
  return false;
}

bool TitleIndex::list(const std::string& key, IndexCursor& cursor, size_t limit, std::vector<Hit>& out) {
  if (cursor.done) return true;
  if (!cursor.started) {
    if (!descend(key, cursor.leaf)) return false;
    cursor.entry = 0;
    cursor.started = true;
  }
  Entry e;
  while (cursor.leaf < leafCount()) {
    const uint8_t* b = block(false, cursor.leaf);
    Leaf leaf;
    if (!b || !leaf.begin(b)) return false;
    uint16_t index = 0;
    e.key.clear();
    e.hit.display.clear();
    while (leaf.next(e)) {
      const uint16_t at = index++;
      if (at < cursor.entry || e.key < key) continue;
      if (!startsWith(e.key, key)) {
        cursor.done = true;
        return true;
      }
      if (out.size() >= limit) {
        cursor.entry = at;
        return true;
      }
      out.push_back(e.hit);
    }
    if (leaf.bad()) return false;
    cursor.leaf++;
    cursor.entry = 0;
  }
  cursor.done = true;
  return true;
}

bool TitleIndex::popular(const std::string& key, size_t limit, std::vector<Hit>& out, uint32_t maxBlocks,
                         size_t openCap) {
  out.clear();
  if (limit == 0) return true;
  // Keys starting with `key` sort below `upper` (key with its last byte + 1).
  std::string upper = key;
  const bool bounded = !upper.empty();
  if (bounded) upper.back() = static_cast<char>(static_cast<uint8_t>(upper.back()) + 1);
  auto overlaps = [&](const std::string& lo, const std::string* hi) {
    return (!bounded || lo < upper) && (!hi || *hi >= key);
  };

  struct Open {
    uint8_t popularity;
    uint8_t inner;
    uint32_t block;
  };
  // Highest popularity first; among equals, leaves in index order.
  auto worse = [](const Open& a, const Open& b) {
    if (a.popularity != b.popularity) return a.popularity < b.popularity;
    if (a.inner != b.inner) return a.inner < b.inner;
    return a.block > b.block;
  };
  std::vector<Open> open;
  open.push_back(Open{0xFF, static_cast<uint8_t>(innerCount() > 0), 0});

  struct Kept {
    Hit hit;
    uint32_t leaf;
    uint16_t index;
  };
  std::vector<Kept> kept;
  // Order of kept entries: more popular, then earlier in the index.
  auto better = [](const Kept& a, const Kept& b) {
    if (a.hit.popularity != b.hit.popularity) return a.hit.popularity > b.hit.popularity;
    if (a.leaf != b.leaf) return a.leaf < b.leaf;
    return a.index < b.index;
  };
  auto worst = [&]() {
    return std::min_element(kept.begin(), kept.end(), [&](const Kept& a, const Kept& b) { return better(b, a); });
  };

  std::vector<Child> kids;
  uint32_t reads = 0;
  while (!open.empty() && reads < maxBlocks) {
    std::pop_heap(open.begin(), open.end(), worse);
    const Open top = open.back();
    open.pop_back();
    if (kept.size() == limit) {
      const Kept& w = *worst();
      if (top.popularity < w.hit.popularity) break;
      if (top.popularity == w.hit.popularity && !top.inner && top.block > w.leaf) continue;
    }
    const uint8_t* b = block(top.inner, top.block);
    reads++;
    if (!b) return false;
    if (top.inner) {
      uint8_t level = 0;
      if (!children(b, level, kids)) return false;
      const bool toInner = level > 1;
      for (size_t i = 0; i < kids.size(); i++) {
        // The last child's upper bound lies in the parent: treat it as open.
        const std::string* hi = i + 1 < kids.size() ? &kids[i + 1].sep : nullptr;
        if (!overlaps(kids[i].sep, hi)) continue;
        if (kept.size() == limit && kids[i].popularity < worst()->hit.popularity) continue;
        if (open.size() >= openCap) break;
        open.push_back(Open{kids[i].popularity, static_cast<uint8_t>(toInner), kids[i].child});
        std::push_heap(open.begin(), open.end(), worse);
      }
      continue;
    }
    Leaf leaf;
    if (!leaf.begin(b)) return false;
    Entry e;
    uint16_t index = 0;
    while (leaf.next(e)) {
      const uint16_t at = index++;
      if (!startsWith(e.key, key)) {
        if (bounded && e.key >= upper) break;
        continue;
      }
      Kept candidate{e.hit, top.block, at};
      // An article counts once, by its first entry in index order.
      auto same =
          std::find_if(kept.begin(), kept.end(), [&](const Kept& k) { return k.hit.article == candidate.hit.article; });
      if (same != kept.end()) {
        if (same->leaf < candidate.leaf || (same->leaf == candidate.leaf && same->index < candidate.index)) continue;
        kept.erase(same);
      }
      if (kept.size() == limit) {
        auto w = worst();
        if (!better(candidate, *w)) continue;
        kept.erase(w);
      }
      kept.push_back(std::move(candidate));
    }
    if (leaf.bad()) return false;
  }
  std::sort(kept.begin(), kept.end(), better);
  out.reserve(kept.size());
  for (auto& k : kept) out.push_back(std::move(k.hit));
  return true;
}

bool TitleIndex::find(const std::string& title, Hit& out) {
  std::string key;
  if (!fold(title.c_str(), key) || key.empty()) return false;
  uint32_t leafIndex = 0;
  if (!descend(key, leafIndex)) return false;
  bool any = false;
  Entry e;
  for (; leafIndex < leafCount(); leafIndex++) {
    const uint8_t* b = block(false, leafIndex);
    Leaf leaf;
    if (!b || !leaf.begin(b)) return false;
    e.key.clear();
    e.hit.display.clear();
    while (leaf.next(e)) {
      if (e.key < key) continue;
      if (e.key != key) return any;
      if (e.hit.display == title) {
        out = e.hit;
        return true;
      }
      if (!any) {
        out = e.hit;
        any = true;
      }
    }
    if (leaf.bad()) return false;
  }
  return any;
}

}  // namespace xwa
