#pragma once

// The title index (spec 6): folding typed text into a search key, listing
// titles in index order, and the most-popular-first search.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "XwaArchive.h"

namespace xwa {

struct Hit {
  std::string display;
  uint32_t article = 0;
  uint8_t flags = 0;  // entry_flag bits
  uint8_t popularity = 0;
  uint32_t anchor = 0;  // valid when flags & entry_flag::ANCHOR
  bool isRedirect() const { return flags & entry_flag::REDIRECT; }
  bool hasAnchor() const { return flags & entry_flag::ANCHOR; }
};

// Where an index-order listing continues.
struct IndexCursor {
  uint32_t leaf = 0;
  uint16_t entry = 0;
  bool started = false;
  bool done = false;
};

class TitleIndex {
 public:
  explicit TitleIndex(Archive& archive) : ar_(archive) {}

  // The search key of typed text (6.1), exactly as the converter folded the
  // titles. False only on a read error.
  bool fold(const char* utf8, std::string& key);

  // Up to `limit` more entries whose key starts with `key`, in index order,
  // continuing from `cursor` (a fresh cursor starts at the first match).
  bool list(const std::string& key, IndexCursor& cursor, size_t limit, std::vector<Hit>& out);

  // The `limit` most popular articles with an entry starting with `key`
  // (6.5), most popular first, each article once. Reads at most `maxBlocks`
  // index blocks and returns what it has found by then; `openCap` bounds the
  // list of subtrees still to open.
  bool popular(const std::string& key, size_t limit, std::vector<Hit>& out, uint32_t maxBlocks = 48,
               size_t openCap = 1024);

  // The entry for exactly this title (the article's own, or a redirect).
  bool find(const std::string& title, Hit& out);

  uint32_t blockReads() const { return blockReads_; }

 private:
  struct Entry {
    Hit hit;
    std::string key;
  };
  // Iterates the entries of one leaf block.
  class Leaf {
   public:
    bool begin(const uint8_t* block);
    bool next(Entry& e);  // false at the end (or on damage: check bad())
    bool bad() const { return bad_; }

   private:
    const uint8_t* p_ = nullptr;
    const uint8_t* end_ = nullptr;
    uint16_t remaining_ = 0;
    bool bad_ = false;
  };
  struct Child {
    std::string sep;
    uint32_t child = 0;
    uint8_t popularity = 0;
  };

  const uint8_t* block(bool inner, uint32_t index);
  bool children(const uint8_t* b, uint8_t& level, std::vector<Child>& out);
  bool descend(const std::string& key, uint32_t& leaf);
  uint32_t leafCount() const;
  uint32_t innerCount() const;

  Archive& ar_;
  uint32_t blockReads_ = 0;
};

}  // namespace xwa
