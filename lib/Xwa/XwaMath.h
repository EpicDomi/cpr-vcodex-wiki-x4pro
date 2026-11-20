#pragma once

// Drawing formulas (spec 8): the MATH_GLYPHS outline table and a small
// anti-aliasing outline rasteriser (exact area coverage, nonzero filling).
// The rasteriser takes any outline in pixel coordinates, so it can also draw
// other vector glyphs.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "XwaArchive.h"
#include "XwaContent.h"

namespace xwa {

class GlyphTable {
 public:
  bool open(Archive& archive);
  bool isOpen() const { return ar_ != nullptr; }
  uint16_t upem() const { return upem_; }
  uint16_t count() const { return count_; }
  const std::vector<uint16_t>& scales() const { return scales_; }
  // The glyph's bytes: i16 bbox[4], then commands up to END.
  bool glyph(uint32_t index, std::vector<uint8_t>& out);

 private:
  Archive* ar_ = nullptr;
  uint64_t base_ = 0;
  uint64_t length_ = 0;
  uint16_t upem_ = 1000;
  uint16_t count_ = 0;
  std::vector<uint16_t> scales_;
  uint64_t offsetsAt_ = 0;  // within the section
  uint64_t dataAt_ = 0;
};

// Rasterises outlines given in pixel coordinates (y down) inside a box.
class Rasterizer {
 public:
  // Receives one row: coverage 0-255 for pixels x .. x + n - 1 of row y.
  using RowFn = void (*)(void* ctx, int y, int x, const uint8_t* coverage, int n);

  // `x0`,`y0`,`w`,`h`: the pixel box drawn into; anything outside is clipped.
  bool begin(int x0, int y0, int w, int h);
  void moveTo(float x, float y);
  void lineTo(float x, float y);
  void quadTo(float cx, float cy, float x, float y);
  void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
  void close();
  // Fills what was added (nonzero winding) and reports rows with ink.
  void fill(RowFn fn, void* ctx);
  void clear() { lines_.clear(); }

 private:
  // Accumulation floats per band: 8 KB, so bands of a few rows.
  static constexpr int BAND_FLOATS = 2048;
  struct Line {
    float x0, y0, x1, y1;
  };
  void add(float x0, float y0, float x1, float y1);

  int x0_ = 0;
  int y0_ = 0;
  int w_ = 0;
  int h_ = 0;
  float startX_ = 0, startY_ = 0, curX_ = 0, curY_ = 0;
  bool open_ = false;
  std::vector<Line> lines_;
  std::vector<float> acc_;
  std::vector<uint8_t> cov_;
};

// Outlines a glyph's commands (font units, y up) into the rasterizer, with
// the glyph origin at (ox, oy) pixels and `sx`,`sy` pixels per font unit.
// `shear` slants it: x moves right by shear × y (synthesised italic).
bool outlineGlyph(const uint8_t* data, size_t len, Rasterizer& r, float ox, float oy, float sx, float sy,
                  float shear = 0.0f);

// Draws a whole formula with its origin (left end of the baseline) at
// (originX, baselineY) and `pxPerUnit` pixels per MATH_GLYPHS font unit.
// Rows go to `fn` item by item (items may overlap: combine by darkening);
// `fn` clips to the screen. Returns false when the formula holds something
// this reader cannot draw: the caller then draws the fallback image (or a
// box) over the same area instead. Check drawable() first to avoid that.
bool drawFormula(const Formula& f, GlyphTable& glyphs, float originX, float baselineY, float pxPerUnit,
                 Rasterizer::RowFn fn, void* ctx);
// True when every item is one this reader knows (parsing only, no drawing).
bool drawable(const Formula& f);

}  // namespace xwa
