#pragma once

// An open XWA archive (format 0.3): the superblock and section directory,
// metadata, fixed-size records, titles, compressed content chunks, the FOLD
// table and DAYS. Everything above this (search, article content, images,
// math) reads through it.
//
// Memory: a small cache of 4 KB file blocks for records and index blocks,
// and a few decompressed chunk slots. Both are sized by Options, so the X4
// (no PSRAM) and the X4 Pro (8 MB PSRAM) use the same code.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "XwaFormat.h"

#if !defined(XWA_HAVE_ZSTD)
#if !defined(ESP_PLATFORM) || defined(BOARD_HAS_PSRAM)
#define XWA_HAVE_ZSTD 1
#else
#define XWA_HAVE_ZSTD 0
#endif
#endif

namespace xwa {

// Where archive bytes come from: a file on the SD card, a host file in tests.
class Source {
 public:
  virtual ~Source() = default;
  virtual uint64_t size() = 0;
  // Reads exactly len bytes at offset; false on any failure.
  virtual bool read(uint64_t offset, void* buf, size_t len) = 0;
};

enum class Status : uint8_t {
  Ok,
  ReadError,           // the card could not be read
  NotXwa,              // not an XWA file
  Damaged,             // a checksum or structure check failed
  Incomplete,          // shorter than it says: an interrupted copy
  TooOld,              // an older draft version: convert it again
  TooNew,              // a newer version than this firmware reads
  NeedsX4Pro,          // full profile (zstd) on a device without zstd
  UnsupportedFeature,  // a required feature this firmware does not know
  NoMemory,
};

struct SectionInfo {
  uint32_t type = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint32_t crc = 0;  // of the section bytes, 0 = not computed
  bool present() const { return type != 0; }
};

struct ArticleRecord {
  uint32_t firstChunk = 0;
  uint16_t offsetInChunk = 0;
  uint16_t chunkCount = 0;
  uint32_t titleRef = 0;
  uint16_t flags = 0;
  uint8_t popularity = 0;
};

struct ChunkInfo {
  uint64_t offset = 0;  // within BLOBS
  uint32_t storedSize = 0;
  uint32_t size = 0;  // uncompressed
};

struct ImageRecord {
  uint64_t offset = 0;  // within BLOBS
  uint32_t size = 0;
  uint8_t encoding = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t kind = 0;
  uint8_t flags = 0;
};

class Archive {
 public:
  struct Options {
    // 4 KB blocks kept for records, titles, the title index and FOLD.
    uint16_t cacheBlocks = 4;
    // Decompressed chunks kept (each 2^chunk_bits bytes).
    uint8_t chunkSlots = 1;
  };

  Archive() = default;
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  // Checks the superblock and loads META, FOLD's block table and, for zstd
  // archives, the dictionary. `src` must outlive the archive.
  Status open(Source* src, const Options& opts);
  void close();
  bool isOpen() const { return src_ != nullptr; }

  uint8_t profile() const { return profile_; }  // 0 compact, 1 full
  uint8_t textCodec() const { return textCodec_; }
  uint32_t chunkBits() const { return chunkBits_; }
  uint32_t chunkSize() const { return 1u << chunkBits_; }
  uint32_t requiredFeatures() const { return required_; }
  uint32_t optionalFeatures() const { return optional_; }
  uint64_t fileSize() const { return fileSize_; }
  uint32_t articleCount() const { return articleCount_; }
  uint32_t chunkCount() const { return chunkCount_; }
  uint32_t imageCount() const { return imageCount_; }
  const SectionInfo& sectionInfo(uint32_t type) const;
  Source* source() const { return src_; }

  const std::vector<std::pair<std::string, std::string>>& meta() const { return meta_; }
  // Empty when absent.
  const std::string& metaValue(const char* key) const;

  bool articleRecord(uint32_t id, ArticleRecord& out);
  bool title(uint32_t id, std::string& out);
  bool titleAt(uint32_t titleRef, std::string& out);
  bool chunkInfo(uint32_t index, ChunkInfo& out);
  bool imageRecord(uint32_t id, ImageRecord& out);
  // Raw bytes of an image's encoded data, uncached (images are read once).
  bool readImage(const ImageRecord& rec, uint32_t offset, void* dst, size_t len);

  // A decompressed chunk from the slot cache (loaded and checked on a miss).
  // The pointer stays valid until another chunk is loaded: compare
  // chunkEpoch() to know whether it still is.
  const uint8_t* chunk(uint32_t index, uint32_t& len);
  uint32_t chunkEpoch() const { return chunkEpoch_; }

  // Article for day `day` (0 = January 1 ... 365 = December 31, with 59 =
  // February 29), or false when the archive has none.
  bool dayArticle(int day, uint32_t& id);
  bool hasDays() const { return sectionInfo(section::DAYS).present(); }

  // FOLD (6.1): the fold of code point cp. `found` false means unchanged.
  bool foldLookup(uint32_t cp, std::string& out, bool& found);

  // Reads through the block cache; for small reads anywhere in the file.
  bool readCached(uint64_t offset, void* dst, size_t len);
  // A whole cached 4 KB block (offset must be block-aligned). `checked`
  // lets callers mark blocks whose own CRC they verified.
  const uint8_t* cachedBlock(uint64_t offset, bool** checked = nullptr);

  static bool zstdAvailable() { return XWA_HAVE_ZSTD != 0; }
#if XWA_HAVE_ZSTD
  // One zstd frame without dictionary (LA2 images of full archives).
  // Returns bytes written, or -1.
  long decompressZstd(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap);
#endif

 private:
  struct CacheBlock {
    uint64_t offset = UINT64_MAX;
    uint8_t* data = nullptr;
    uint32_t lastUse = 0;
    bool checked = false;
  };
  struct ChunkSlot {
    uint32_t index = UINT32_MAX;
    uint8_t* data = nullptr;
    uint32_t len = 0;
    uint32_t lastUse = 0;
  };

  Status loadMeta();
  Status loadFold();
  Status loadDictionary();
  bool decompressChunk(uint32_t index, uint8_t* out, uint32_t& outLen);
  bool readSection(const SectionInfo& s, std::vector<uint8_t>& out, bool checkCrc);

  Source* src_ = nullptr;
  uint64_t fileSize_ = 0;
  uint8_t profile_ = 0;
  uint8_t textCodec_ = 0;
  uint32_t chunkBits_ = 15;
  uint32_t required_ = 0;
  uint32_t optional_ = 0;
  uint32_t articleCount_ = 0;
  uint32_t chunkCount_ = 0;
  uint32_t imageCount_ = 0;
  std::vector<SectionInfo> sections_;
  std::vector<std::pair<std::string, std::string>> meta_;

  std::vector<CacheBlock> cache_;
  std::vector<ChunkSlot> slots_;
  uint32_t useCounter_ = 0;
  uint32_t chunkEpoch_ = 0;

  // FOLD: record count, and the first code point of each 4 KB block.
  uint32_t foldCount_ = 0;
  std::vector<uint32_t> foldBlockFirst_;

  // DAYS, loaded on first use.
  std::vector<uint32_t> days_;
  bool daysLoaded_ = false;

#if XWA_HAVE_ZSTD
  void* zstdDict_ = nullptr;  // dictionary bytes (ZSTD_DDict refers to them)
  void* zstdDDict_ = nullptr;
  void* zstdDCtx_ = nullptr;
  uint8_t* zstdIn_ = nullptr;  // stored chunk bytes
  size_t zstdInCap_ = 0;
#endif
};

// Decompression helpers shared by chunks and LA2 images.
namespace codec {
// Raw deflate from `src`, pulled in pieces through `fill`, into `dst` (which
// receives the whole output, so no window is needed). Returns bytes written
// or -1.
using FillFn = size_t (*)(void* ctx, const uint8_t** data);
long inflateAll(FillFn fill, void* ctx, uint8_t* dst, size_t dstCap);
}  // namespace codec

}  // namespace xwa
