#pragma once

// LA2 line-art images (spec 9.2): rows of 1- or 2-bit ink levels (0 paper,
// max black), raw deflate with back-references of at most 4 KB (compact
// archives) or zstd (full archives). Rows come out one at a time, so the X4
// decodes any size of image through a 4 KB window.

#include <cstddef>
#include <cstdint>

#include "XwaArchive.h"

struct tinfl_decompressor_tag;

namespace xwa {

class La2Reader {
 public:
  La2Reader() = default;
  ~La2Reader() { end(); }
  La2Reader(const La2Reader&) = delete;
  La2Reader& operator=(const La2Reader&) = delete;

  bool begin(Archive& archive, const ImageRecord& rec);
  void end();
  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }
  uint8_t bpp() const { return bpp_; }
  uint8_t maxLevel() const { return static_cast<uint8_t>((1 << bpp_) - 1); }
  // The next row as ink levels, one byte per pixel (width() bytes).
  bool row(uint8_t* levels);

 private:
  bool fillPacked(uint8_t* dst, size_t len);
  bool refill();

  Archive* ar_ = nullptr;
  ImageRecord rec_{};
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  uint8_t bpp_ = 0;
  uint8_t codec_ = 0;
  uint32_t stride_ = 0;
  uint16_t rowsDone_ = 0;
  uint8_t* packedRow_ = nullptr;

  // deflate: tinfl over a 4 KB ring, fed from the archive in small reads.
  tinfl_decompressor_tag* inflater_ = nullptr;
  uint8_t* ring_ = nullptr;
  size_t ringPos_ = 0;    // where tinfl writes next
  size_t ringReady_ = 0;  // decoded bytes from ringPos_ - ringReady_ not yet taken
  uint8_t* in_ = nullptr;
  size_t inPos_ = 0;
  size_t inLen_ = 0;
  uint32_t srcPos_ = 0;  // next byte of the image data to read
  bool streamDone_ = false;

  // zstd: the whole image, decoded at once (X4 Pro, PSRAM).
  uint8_t* whole_ = nullptr;
  size_t wholePos_ = 0;
};

}  // namespace xwa
