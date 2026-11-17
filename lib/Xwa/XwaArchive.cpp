#include "XwaArchive.h"

#include <InflateStream.h>

#include <algorithm>
#include <memory>

#include "XwaCrc.h"

#if XWA_HAVE_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

namespace xwa {

namespace {

const SectionInfo kNoSection{};
const std::string kEmpty;

constexpr uint32_t SECTIONS_AT = 40;
constexpr uint32_t SECTION_ENTRY = 32;
constexpr uint32_t SUPERBLOCK_CRC_AT = SUPERBLOCK_SIZE - 4;
constexpr uint32_t STAGING = 4096;

#if XWA_HAVE_ZSTD
void* zalloc(void*, size_t n) { return bigAlloc(n); }
void zfree(void*, void* p) { bigFree(p); }
constexpr ZSTD_customMem kZstdMem = {zalloc, zfree, nullptr};
#endif

struct DeflateFill {
  Source* src;
  uint64_t pos;
  uint64_t end;
  uint32_t crc;
  uint8_t* buf;
  bool failed;
};

size_t fillFromSource(void* ctx, const uint8_t** data) {
  auto* f = static_cast<DeflateFill*>(ctx);
  if (f->failed || f->pos >= f->end) return 0;
  const size_t n = static_cast<size_t>(std::min<uint64_t>(STAGING, f->end - f->pos));
  if (!f->src->read(f->pos, f->buf, n)) {
    f->failed = true;
    return 0;
  }
  f->crc = crc32(f->crc, f->buf, n);
  f->pos += n;
  *data = f->buf;
  return n;
}

}  // namespace

namespace codec {
long inflateAll(FillFn fill, void* ctx, uint8_t* dst, size_t dstCap) {
  InflateStream s;
  if (!s.init(false)) return -1;
  s.setFill(fill, ctx);
  size_t produced = 0;
  const auto status = s.readAtMost(dst, dstCap, &produced);
  if (status == InflateStream::Status::Error) return -1;
  return static_cast<long>(produced);
}
}  // namespace codec

Archive::~Archive() { close(); }

void Archive::close() {
  for (auto& b : cache_) bigFree(b.data);
  cache_.clear();
  for (auto& s : slots_) bigFree(s.data);
  slots_.clear();
#if XWA_HAVE_ZSTD
  if (zstdDDict_) ZSTD_freeDDict(static_cast<ZSTD_DDict*>(zstdDDict_));
  if (zstdDCtx_) ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(zstdDCtx_));
  bigFree(zstdDict_);
  bigFree(zstdIn_);
  zstdDDict_ = zstdDCtx_ = zstdDict_ = nullptr;
  zstdIn_ = nullptr;
  zstdInCap_ = 0;
#endif
  sections_.clear();
  meta_.clear();
  foldBlockFirst_.clear();
  foldCount_ = 0;
  days_.clear();
  daysLoaded_ = false;
  src_ = nullptr;
}

const SectionInfo& Archive::sectionInfo(uint32_t type) const {
  for (const auto& s : sections_) {
    if (s.type == type) return s;
  }
  return kNoSection;
}

const std::string& Archive::metaValue(const char* key) const {
  for (const auto& kv : meta_) {
    if (kv.first == key) return kv.second;
  }
  return kEmpty;
}

Status Archive::open(Source* src, const Options& opts) {
  close();
  if (!src) return Status::ReadError;
  const uint64_t size = src->size();
  if (size < SUPERBLOCK_SIZE) return size == 0 ? Status::ReadError : Status::NotXwa;
  std::unique_ptr<uint8_t[]> sb(new (std::nothrow) uint8_t[SUPERBLOCK_SIZE]);
  if (!sb) return Status::NoMemory;
  if (!src->read(0, sb.get(), SUPERBLOCK_SIZE)) return Status::ReadError;
  const uint8_t* b = sb.get();
  if (std::memcmp(b, MAGIC, sizeof(MAGIC)) != 0) return Status::NotXwa;
  if (crc32(0, b, SUPERBLOCK_CRC_AT) != le32(b + SUPERBLOCK_CRC_AT)) return Status::Damaged;
  const uint16_t major = le16(b + 8);
  const uint16_t minor = le16(b + 10);
  if (major > VERSION_MAJOR || (major == VERSION_MAJOR && minor > VERSION_MINOR)) return Status::TooNew;
  if (major < VERSION_MAJOR || minor < VERSION_MINOR) return Status::TooOld;
  if (le32(b + 12) != SUPERBLOCK_SIZE) return Status::Damaged;
  const uint64_t declared = le64(b + 16);
  if (declared > size) return Status::Incomplete;
  if (declared < size) return Status::Damaged;
  fileSize_ = declared;
  required_ = le32(b + 24);
  optional_ = le32(b + 28);
  profile_ = b[32];
  textCodec_ = b[33];
  chunkBits_ = b[35];
  constexpr uint32_t known = required::ZSTD_TEXT | required::MATH | required::LINE_ART | required::JPEG;
  if (required_ & ~known) return Status::UnsupportedFeature;
  if (profile_ > 1 || textCodec_ > 1) return Status::UnsupportedFeature;
  if (chunkBits_ < MIN_CHUNK_BITS || chunkBits_ > MAX_CHUNK_BITS) return Status::UnsupportedFeature;
  if (!XWA_HAVE_ZSTD && (profile_ == 1 || textCodec_ == 1 || (required_ & required::ZSTD_TEXT))) {
    return Status::NeedsX4Pro;
  }
  const uint32_t count = le32(b + 36);
  if (count > (SUPERBLOCK_CRC_AT - SECTIONS_AT) / SECTION_ENTRY) return Status::Damaged;
  sections_.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* e = b + SECTIONS_AT + i * SECTION_ENTRY;
    SectionInfo s;
    s.type = le32(e);
    s.offset = le64(e + 8);
    s.length = le64(e + 16);
    s.crc = le32(e + 24);
    if (s.type == 0) continue;
    if (s.offset > fileSize_ || s.length > fileSize_ - s.offset) return Status::Damaged;
    if (sectionInfo(s.type).present()) return Status::Damaged;  // a type appears at most once
    sections_.push_back(s);
  }
  sb.reset();
  for (uint32_t needed : {section::META, section::ARTICLES, section::TITLES, section::TINDEX_LEAF, section::CHUNK_TABLE,
                          section::BLOBS, section::FOLD}) {
    if (!sectionInfo(needed).present()) return Status::Damaged;
  }
  articleCount_ = static_cast<uint32_t>(sectionInfo(section::ARTICLES).length / 16);
  chunkCount_ = static_cast<uint32_t>(sectionInfo(section::CHUNK_TABLE).length / 12);
  imageCount_ = static_cast<uint32_t>(sectionInfo(section::IMAGE_TABLE).length / 16);
  if (sectionInfo(section::TINDEX_LEAF).length % BLOCK_SIZE || sectionInfo(section::TINDEX_INNER).length % BLOCK_SIZE) {
    return Status::Damaged;
  }

  src_ = src;
  const uint16_t blocks = std::max<uint16_t>(opts.cacheBlocks, 2);
  cache_.resize(blocks);
  for (auto& c : cache_) {
    c.data = static_cast<uint8_t*>(bigAlloc(BLOCK_SIZE));
    if (!c.data) {
      close();
      return Status::NoMemory;
    }
  }
  slots_.resize(std::max<uint8_t>(opts.chunkSlots, 1));  // allocated on first use

  Status st = loadMeta();
  if (st == Status::Ok) st = loadFold();
  if (st == Status::Ok) st = loadDictionary();
  if (st != Status::Ok) close();
  return st;
}

bool Archive::readSection(const SectionInfo& s, std::vector<uint8_t>& out, bool checkCrc) {
  out.clear();
  if (!s.present()) return false;
  if (s.length > 4u * 1024 * 1024) return false;
  out.resize(static_cast<size_t>(s.length));
  if (!out.empty() && !src_->read(s.offset, out.data(), out.size())) return false;
  return !checkCrc || s.crc == 0 || crc32(0, out.data(), out.size()) == s.crc;
}

Status Archive::loadMeta() {
  std::vector<uint8_t> buf;
  if (!readSection(sectionInfo(section::META), buf, true)) return Status::Damaged;
  const uint8_t* p = buf.data();
  const uint8_t* end = p + buf.size();
  uint32_t count = 0;
  if (!getVarint32(p, end, count) || count > 4096) return Status::Damaged;
  meta_.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    std::string kv[2];
    for (auto& s : kv) {
      uint32_t len = 0;
      if (!getVarint32(p, end, len) || len > static_cast<size_t>(end - p)) return Status::Damaged;
      s.assign(reinterpret_cast<const char*>(p), len);
      p += len;
    }
    meta_.emplace_back(std::move(kv[0]), std::move(kv[1]));
  }
  return Status::Ok;
}

Status Archive::loadFold() {
  const SectionInfo& s = sectionInfo(section::FOLD);
  if (s.length < 8) return Status::Damaged;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[BLOCK_SIZE]);
  if (!buf) return Status::NoMemory;
  uint32_t crc = 0;
  for (uint64_t at = 0; at < s.length; at += BLOCK_SIZE) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(BLOCK_SIZE, s.length - at));
    if (!src_->read(s.offset + at, buf.get(), n)) return Status::ReadError;
    crc = crc32(crc, buf.get(), n);
    if (at == 0) {
      foldCount_ = le32(buf.get());
      if (8ull + 8ull * foldCount_ > s.length) return Status::Damaged;
    }
    // The first record that starts in this block (records never straddle).
    const uint64_t recordsEnd = 8ull + 8ull * foldCount_;
    const uint64_t first = at == 0 ? 8 : at;
    if (first + 8 <= recordsEnd) foldBlockFirst_.push_back(le32(buf.get() + (first - at)));
  }
  if (s.crc != 0 && crc != s.crc) return Status::Damaged;
  return Status::Ok;
}

Status Archive::loadDictionary() {
#if XWA_HAVE_ZSTD
  if (textCodec_ != 1) return Status::Ok;
  auto* dctx = ZSTD_createDCtx_advanced(kZstdMem);
  if (!dctx) return Status::NoMemory;
  zstdDCtx_ = dctx;
  const SectionInfo& d = sectionInfo(section::ZSTD_DICT);
  if (!d.present() || d.length == 0) return Status::Ok;
  if (d.length > 2u * 1024 * 1024) return Status::Damaged;
  zstdDict_ = bigAlloc(static_cast<size_t>(d.length));
  if (!zstdDict_) return Status::NoMemory;
  if (!src_->read(d.offset, zstdDict_, static_cast<size_t>(d.length))) return Status::ReadError;
  if (d.crc != 0 && crc32(0, zstdDict_, static_cast<size_t>(d.length)) != d.crc) return Status::Damaged;
  zstdDDict_ =
      ZSTD_createDDict_advanced(zstdDict_, static_cast<size_t>(d.length), ZSTD_dlm_byRef, ZSTD_dct_auto, kZstdMem);
  return zstdDDict_ ? Status::Ok : Status::NoMemory;
#else
  return Status::Ok;
#endif
}

const uint8_t* Archive::cachedBlock(uint64_t offset, bool** checked) {
  if (!src_ || offset >= fileSize_) return nullptr;
  CacheBlock* victim = &cache_[0];
  for (auto& c : cache_) {
    if (c.offset == offset) {
      c.lastUse = ++useCounter_;
      if (checked) *checked = &c.checked;
      return c.data;
    }
    if (c.lastUse < victim->lastUse) victim = &c;
  }
  const size_t n = static_cast<size_t>(std::min<uint64_t>(BLOCK_SIZE, fileSize_ - offset));
  victim->offset = UINT64_MAX;
  if (!src_->read(offset, victim->data, n)) return nullptr;
  if (n < BLOCK_SIZE) std::memset(victim->data + n, 0, BLOCK_SIZE - n);
  victim->offset = offset;
  victim->lastUse = ++useCounter_;
  victim->checked = false;
  if (checked) *checked = &victim->checked;
  return victim->data;
}

bool Archive::readCached(uint64_t offset, void* dst, size_t len) {
  auto* out = static_cast<uint8_t*>(dst);
  if (offset > fileSize_ || len > fileSize_ - offset) return false;
  while (len > 0) {
    const uint64_t blockAt = offset & ~static_cast<uint64_t>(BLOCK_SIZE - 1);
    const uint8_t* block = cachedBlock(blockAt);
    if (!block) return false;
    const size_t in = static_cast<size_t>(offset - blockAt);
    const size_t n = std::min(len, static_cast<size_t>(BLOCK_SIZE) - in);
    std::memcpy(out, block + in, n);
    out += n;
    offset += n;
    len -= n;
  }
  return true;
}

bool Archive::articleRecord(uint32_t id, ArticleRecord& out) {
  if (id >= articleCount_) return false;
  uint8_t r[16];
  if (!readCached(sectionInfo(section::ARTICLES).offset + 16ull * id, r, sizeof(r))) return false;
  out.firstChunk = le32(r);
  out.offsetInChunk = le16(r + 4);
  out.chunkCount = le16(r + 6);
  out.titleRef = le32(r + 8);
  out.flags = le16(r + 12);
  out.popularity = r[14];
  return out.firstChunk < chunkCount_ && out.offsetInChunk < chunkSize();
}

bool Archive::titleAt(uint32_t titleRef, std::string& out) {
  const SectionInfo& s = sectionInfo(section::TITLES);
  if (titleRef >= s.length) return false;
  uint8_t head[5];
  const size_t headLen = static_cast<size_t>(std::min<uint64_t>(sizeof(head), s.length - titleRef));
  if (!readCached(s.offset + titleRef, head, headLen)) return false;
  const uint8_t* p = head;
  uint32_t len = 0;
  if (!getVarint32(p, head + headLen, len)) return false;
  const uint64_t at = titleRef + static_cast<uint64_t>(p - head);
  if (len > 1024 || at + len > s.length) return false;
  out.resize(len);
  return len == 0 || readCached(s.offset + at, &out[0], len);
}

bool Archive::title(uint32_t id, std::string& out) {
  ArticleRecord r;
  return articleRecord(id, r) && titleAt(r.titleRef, out);
}

bool Archive::chunkInfo(uint32_t index, ChunkInfo& out) {
  if (index >= chunkCount_) return false;
  uint8_t r[12];
  if (!readCached(sectionInfo(section::CHUNK_TABLE).offset + 12ull * index, r, sizeof(r))) return false;
  out.offset = le48(r);
  out.storedSize = le24(r + 6);
  out.size = le24(r + 9);
  const SectionInfo& blobs = sectionInfo(section::BLOBS);
  if (out.storedSize < 5 || out.offset > blobs.length || out.storedSize > blobs.length - out.offset) return false;
  const bool last = index + 1 == chunkCount_;
  return last ? (out.size >= 1 && out.size <= chunkSize()) : out.size == chunkSize();
}

bool Archive::imageRecord(uint32_t id, ImageRecord& out) {
  if (id >= imageCount_) return false;
  uint8_t r[16];
  if (!readCached(sectionInfo(section::IMAGE_TABLE).offset + 16ull * id, r, sizeof(r))) return false;
  out.offset = le48(r);
  out.size = le24(r + 6);
  out.encoding = r[9];
  out.width = le16(r + 10);
  out.height = le16(r + 12);
  out.kind = r[14];
  out.flags = r[15];
  const SectionInfo& blobs = sectionInfo(section::BLOBS);
  return out.size > 0 && out.offset <= blobs.length && out.size <= blobs.length - out.offset;
}

bool Archive::readImage(const ImageRecord& rec, uint32_t offset, void* dst, size_t len) {
  if (offset > rec.size || len > rec.size - offset) return false;
  return src_->read(sectionInfo(section::BLOBS).offset + rec.offset + offset, dst, len);
}

bool Archive::decompressChunk(uint32_t index, uint8_t* out, uint32_t& outLen) {
  ChunkInfo ci;
  if (!chunkInfo(index, ci)) return false;
  const uint64_t at = sectionInfo(section::BLOBS).offset + ci.offset;
  const uint32_t packed = ci.storedSize - 4;
  uint8_t crcBytes[4];
  if (!src_->read(at + packed, crcBytes, 4)) return false;
  const uint32_t wantCrc = le32(crcBytes);
  long n = -1;
  if (textCodec_ == 0) {
    std::unique_ptr<uint8_t[]> staging(new (std::nothrow) uint8_t[STAGING]);
    if (!staging) return false;
    DeflateFill f{src_, at, at + packed, 0, staging.get(), false};
    n = codec::inflateAll(fillFromSource, &f, out, chunkSize());
    // Check every stored byte, even past where the stream ended.
    while (!f.failed && f.pos < f.end) {
      const uint8_t* ignored = nullptr;
      fillFromSource(&f, &ignored);
    }
    if (f.failed || f.crc != wantCrc) return false;
  } else {
#if XWA_HAVE_ZSTD
    if (zstdInCap_ < packed) {
      bigFree(zstdIn_);
      zstdInCap_ = 0;
      zstdIn_ = static_cast<uint8_t*>(bigAlloc(packed));
      if (!zstdIn_) return false;
      zstdInCap_ = packed;
    }
    if (!src_->read(at, zstdIn_, packed) || crc32(0, zstdIn_, packed) != wantCrc) return false;
    const size_t r = ZSTD_decompress_usingDDict(static_cast<ZSTD_DCtx*>(zstdDCtx_), out, chunkSize(), zstdIn_, packed,
                                                static_cast<const ZSTD_DDict*>(zstdDDict_));
    n = ZSTD_isError(r) ? -1 : static_cast<long>(r);
#else
    return false;
#endif
  }
  if (n < 0 || static_cast<uint32_t>(n) != ci.size) return false;
  outLen = ci.size;
  return true;
}

const uint8_t* Archive::chunk(uint32_t index, uint32_t& len) {
  ChunkSlot* victim = &slots_[0];
  for (auto& s : slots_) {
    if (s.index == index && s.data) {
      s.lastUse = ++useCounter_;
      len = s.len;
      return s.data;
    }
    if (s.lastUse < victim->lastUse) victim = &s;
  }
  if (!victim->data) {
    victim->data = static_cast<uint8_t*>(bigAlloc(chunkSize()));
    if (!victim->data) return nullptr;
  }
  chunkEpoch_++;
  victim->index = UINT32_MAX;
  uint32_t n = 0;
  if (!decompressChunk(index, victim->data, n)) return nullptr;
  victim->index = index;
  victim->len = n;
  victim->lastUse = ++useCounter_;
  len = n;
  return victim->data;
}

#if XWA_HAVE_ZSTD
long Archive::decompressZstd(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
  if (!zstdDCtx_) {
    zstdDCtx_ = ZSTD_createDCtx_advanced(kZstdMem);
    if (!zstdDCtx_) return -1;
  }
  const size_t r = ZSTD_decompress_usingDDict(static_cast<ZSTD_DCtx*>(zstdDCtx_), dst, dstCap, src, srcLen, nullptr);
  return ZSTD_isError(r) ? -1 : static_cast<long>(r);
}
#endif

bool Archive::dayArticle(int day, uint32_t& id) {
  if (!daysLoaded_) {
    daysLoaded_ = true;
    std::vector<uint8_t> buf;
    if (readSection(sectionInfo(section::DAYS), buf, true) && buf.size() == DAYS_IN_YEAR * 4u) {
      days_.resize(DAYS_IN_YEAR);
      for (int i = 0; i < DAYS_IN_YEAR; i++) days_[i] = le32(buf.data() + 4 * i);
    }
  }
  if (day < 0 || day >= static_cast<int>(days_.size())) return false;
  id = days_[day];
  return id != NO_ARTICLE && id < articleCount_;
}

bool Archive::foldLookup(uint32_t cp, std::string& out, bool& found) {
  found = false;
  if (foldCount_ == 0 || foldBlockFirst_.empty() || cp < foldBlockFirst_[0]) return true;
  const SectionInfo& s = sectionInfo(section::FOLD);
  // The last block whose first code point is <= cp.
  size_t lo = 0;
  size_t hi = foldBlockFirst_.size();
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (foldBlockFirst_[mid] <= cp) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const uint8_t* block = cachedBlock(s.offset + static_cast<uint64_t>(lo) * BLOCK_SIZE);
  if (!block) return false;
  // Records in this block: those starting in it.
  const uint32_t firstRec = lo == 0 ? 0 : static_cast<uint32_t>(lo * 512 - 1);
  const uint32_t lastRec = std::min<uint32_t>(foldCount_, static_cast<uint32_t>((lo + 1) * 512 - 1));
  const uint64_t blockStart = static_cast<uint64_t>(lo) * BLOCK_SIZE;
  uint32_t a = firstRec;
  uint32_t b = lastRec;
  while (a < b) {
    const uint32_t m = (a + b) / 2;
    const uint8_t* rec = block + (8ull + 8ull * m - blockStart);
    const uint32_t mcp = le32(rec);
    if (mcp == cp) {
      const uint8_t len = rec[4];
      found = true;
      if (len <= 3) {
        out.append(reinterpret_cast<const char*>(rec + 5), len);
        return true;
      }
      const uint64_t pool = 8ull + 8ull * foldCount_ + le24(rec + 5);
      if (pool + len > s.length) return false;
      char tmp[64];
      if (len > sizeof(tmp)) return false;
      if (!readCached(s.offset + pool, tmp, len)) return false;
      out.append(tmp, len);
      return true;
    }
    if (mcp < cp) {
      a = m + 1;
    } else {
      b = m;
    }
  }
  return true;
}

}  // namespace xwa
