#include "XwaLa2.h"

#include <MinizConfig.h>

#include <algorithm>
#include <cstdlib>

namespace xwa {

namespace {
constexpr size_t HEADER = 6;
constexpr size_t RING = 4096;  // compact LA2 back-references reach at most 4 KB
constexpr size_t INPUT = 1024;
constexpr uint8_t CODEC_DEFLATE = 0;
constexpr uint8_t CODEC_ZSTD = 1;
}  // namespace

void La2Reader::end() {
  std::free(inflater_);
  std::free(ring_);
  std::free(in_);
  std::free(packedRow_);
  bigFree(whole_);
  inflater_ = nullptr;
  ring_ = nullptr;
  in_ = nullptr;
  packedRow_ = nullptr;
  whole_ = nullptr;
  ar_ = nullptr;
}

bool La2Reader::begin(Archive& archive, const ImageRecord& rec) {
  end();
  if (rec.encoding != image_encoding::LINE_ART || rec.size <= HEADER) return false;
  uint8_t head[HEADER];
  if (!archive.readImage(rec, 0, head, HEADER)) return false;
  width_ = le16(head);
  height_ = le16(head + 2);
  bpp_ = head[4];
  codec_ = head[5];
  if ((bpp_ != 1 && bpp_ != 2) || width_ == 0 || height_ == 0) return false;
  stride_ = (static_cast<uint32_t>(width_) * bpp_ + 7) / 8;
  ar_ = &archive;
  rec_ = rec;
  rowsDone_ = 0;
  srcPos_ = HEADER;
  packedRow_ = static_cast<uint8_t*>(std::malloc(stride_));
  if (!packedRow_) return false;
  if (codec_ == CODEC_DEFLATE) {
    inflater_ = static_cast<tinfl_decompressor_tag*>(std::malloc(sizeof(tinfl_decompressor)));
    ring_ = static_cast<uint8_t*>(std::malloc(RING));
    in_ = static_cast<uint8_t*>(std::malloc(INPUT));
    if (!inflater_ || !ring_ || !in_) {
      end();
      return false;
    }
    tinfl_init(reinterpret_cast<tinfl_decompressor*>(inflater_));
    ringPos_ = ringReady_ = inPos_ = inLen_ = 0;
    streamDone_ = false;
    return true;
  }
#if XWA_HAVE_ZSTD
  if (codec_ == CODEC_ZSTD) {
    const size_t packed = rec.size - HEADER;
    const size_t total = static_cast<size_t>(stride_) * height_;
    uint8_t* src = static_cast<uint8_t*>(bigAlloc(packed));
    whole_ = static_cast<uint8_t*>(bigAlloc(total));
    bool ok = src && whole_ && archive.readImage(rec, HEADER, src, packed) &&
              archive.decompressZstd(src, packed, whole_, total) == static_cast<long>(total);
    bigFree(src);
    wholePos_ = 0;
    if (!ok) end();
    return ok;
  }
#endif
  end();
  return false;
}

bool La2Reader::refill() {
  const uint32_t left = rec_.size - srcPos_;
  const size_t n = std::min<size_t>(INPUT, left);
  if (n == 0) return false;
  if (!ar_->readImage(rec_, srcPos_, in_, n)) return false;
  srcPos_ += static_cast<uint32_t>(n);
  inPos_ = 0;
  inLen_ = n;
  return true;
}

bool La2Reader::fillPacked(uint8_t* dst, size_t len) {
  auto* r = reinterpret_cast<tinfl_decompressor*>(inflater_);
  while (len > 0) {
    if (ringReady_ > 0) {
      // Take decoded bytes in ring order; they sit just before ringPos_.
      const size_t start = (ringPos_ - ringReady_) & (RING - 1);
      const size_t n = std::min({len, ringReady_, RING - start});
      std::memcpy(dst, ring_ + start, n);
      dst += n;
      len -= n;
      ringReady_ -= n;
      continue;
    }
    if (streamDone_) return false;
    if (inPos_ >= inLen_ && srcPos_ < rec_.size && !refill()) return false;
    size_t inSize = inLen_ - inPos_;
    size_t outSize = RING - ringPos_;
    const mz_uint32 flags = srcPos_ < rec_.size ? TINFL_FLAG_HAS_MORE_INPUT : 0;
    const tinfl_status st = tinfl_decompress(r, in_ + inPos_, &inSize, ring_, ring_ + ringPos_, &outSize, flags);
    inPos_ += inSize;
    ringPos_ = (ringPos_ + outSize) & (RING - 1);
    ringReady_ = outSize;
    if (st == TINFL_STATUS_DONE) {
      streamDone_ = true;
    } else if (st < 0) {
      return false;
    } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && inSize == 0 && outSize == 0 && srcPos_ >= rec_.size) {
      return false;  // truncated
    }
  }
  return true;
}

bool La2Reader::row(uint8_t* levels) {
  if (!ar_ || rowsDone_ >= height_) return false;
  const uint8_t* packed = nullptr;
  if (whole_) {
    packed = whole_ + wholePos_;
    wholePos_ += stride_;
  } else {
    if (!fillPacked(packedRow_, stride_)) return false;
    packed = packedRow_;
  }
  rowsDone_++;
  if (bpp_ == 2) {
    for (uint16_t x = 0; x < width_; x++) levels[x] = (packed[x >> 2] >> (6 - 2 * (x & 3))) & 3;
  } else {
    for (uint16_t x = 0; x < width_; x++) levels[x] = (packed[x >> 3] >> (7 - (x & 7))) & 1;
  }
  return true;
}

}  // namespace xwa
