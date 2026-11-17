#pragma once

// Pictures and formulas, decoded once per page into small 2-bit pixmaps
// (four greys, as the panel shows them) and drawn from there in every
// render pass: the black-and-white pass and the grey passes of
// anti-aliasing, which the reader repeats strip by strip.

#include <cstddef>
#include <cstdint>
#include <memory>

class GfxRenderer;
class WikiSession;

namespace xwa {
struct ImageRecord;
}

namespace wiki {

class Pixmap {
 public:
  Pixmap() = default;
  ~Pixmap() { release(); }
  Pixmap(const Pixmap&) = delete;
  Pixmap& operator=(const Pixmap&) = delete;
  Pixmap(Pixmap&& o) noexcept;
  Pixmap& operator=(Pixmap&& o) noexcept;

  // White, w × h. False when memory runs out.
  bool allocate(int w, int h);
  void release();
  bool valid() const { return data_ != nullptr; }
  int width() const { return w_; }
  int height() const { return h_; }
  // Pixel levels: 0 black, 1 dark grey, 2 light grey, 3 white.
  uint8_t get(int x, int y) const { return (data_[(y * stride_) + (x >> 2)] >> (6 - 2 * (x & 3))) & 3; }
  void set(int x, int y, uint8_t v) {
    uint8_t& b = data_[(y * stride_) + (x >> 2)];
    const int s = 6 - 2 * (x & 3);
    b = static_cast<uint8_t>((b & ~(3 << s)) | ((v & 3) << s));
  }
  // Draws at (x, y) in screen coordinates, in the renderer's current pass.
  void draw(GfxRenderer& renderer, int x, int y) const;

 private:
  uint8_t* data_ = nullptr;
  int w_ = 0;
  int h_ = 0;
  int stride_ = 0;
};

// Decodes archive image `id` scaled to w × h. Photos are dithered; line art
// keeps its four levels (downscaled edges become grey).
bool decodePicture(WikiSession& session, uint32_t id, int w, int h, Pixmap& out);

// The part [viewX, viewX + viewW) × [viewY, viewY + viewH) of the picture
// scaled to sw × sh, into a viewW × viewH pixmap.
bool decodePictureRegion(WikiSession& session, uint32_t id, int sw, int sh, int viewX, int viewY, int viewW, int viewH,
                         Pixmap& out);

// Draws a picture region at 1:1 or any scale straight into the renderer:
// `view` is the part of the scaled picture (sw × sh) shown at (x, y). Used
// by the zoom viewer, where the whole scaled picture would not fit in RAM.
bool drawPictureRegion(GfxRenderer& renderer, WikiSession& session, uint32_t id, int sw, int sh, int viewX, int viewY,
                       int viewW, int viewH, int x, int y);

// Rasterises a formula record into a w × h pixmap (anti-aliased). Falls
// back to the formula's picture when it holds items this reader does not
// know; false when there is neither.
bool renderFormula(WikiSession& session, uint32_t article, uint32_t pos, uint32_t len, int w, int h, Pixmap& out);

}  // namespace wiki
