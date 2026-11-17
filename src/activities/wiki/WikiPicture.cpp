#include "WikiPicture.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <XwaContent.h>
#include <XwaLa2.h>
#include <XwaMath.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/DitherUtils.h"
#include "WikiSession.h"

namespace wiki {

// ------------------------------------------------------------------ pixmap

Pixmap::Pixmap(Pixmap&& o) noexcept : data_(o.data_), w_(o.w_), h_(o.h_), stride_(o.stride_) {
  o.data_ = nullptr;
  o.w_ = o.h_ = o.stride_ = 0;
}

Pixmap& Pixmap::operator=(Pixmap&& o) noexcept {
  if (this != &o) {
    release();
    data_ = o.data_;
    w_ = o.w_;
    h_ = o.h_;
    stride_ = o.stride_;
    o.data_ = nullptr;
    o.w_ = o.h_ = o.stride_ = 0;
  }
  return *this;
}

bool Pixmap::allocate(int w, int h) {
  release();
  if (w <= 0 || h <= 0) return false;
  stride_ = (w + 3) / 4;
  const size_t bytes = static_cast<size_t>(stride_) * h;
  data_ = static_cast<uint8_t*>(xwa::bigAlloc(bytes));
  if (!data_) {
    LOG_ERR("WIKI", "No memory for a %dx%d picture", w, h);
    stride_ = 0;
    return false;
  }
  std::memset(data_, 0xFF, bytes);
  w_ = w;
  h_ = h;
  return true;
}

void Pixmap::release() {
  xwa::bigFree(data_);
  data_ = nullptr;
  w_ = h_ = stride_ = 0;
}

void Pixmap::draw(GfxRenderer& renderer, int x, int y) const {
  if (!data_) return;
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const int x0 = std::max(0, -x);
  const int x1 = std::min(w_, sw - x);
  const int y0 = std::max(0, -y);
  const int y1 = std::min(h_, sh - y);
  if (x0 >= x1 || y0 >= y1) return;
  DirectPixelWriter pw;
  pw.init(renderer);
  for (int row = y0; row < y1; row++) {
    pw.beginRow(y + row);
    int cs = 0;
    int ce = 0;
    pw.bandColRange(x, w_, cs, ce);
    cs = std::max(cs, x0);
    ce = std::min(ce, x1);
    for (int col = cs; col < ce; col++) {
      const uint8_t v = get(col, row);
      if (v != 3) pw.writePixel(x + col, v);
    }
  }
}

// ------------------------------------------------------------------ scaling

namespace {

// Resamples a stream of grey rows (0 black .. 255 white) from sw × sh to
// dw × dh: area averaging when shrinking, so thin lines turn grey rather
// than vanish; linear interpolation when enlarging.
class RowScaler {
 public:
  using Sink = void (*)(void* ctx, int y, const uint8_t* row, int w);

  bool begin(int sw, int sh, int dw, int dh, Sink sink, void* ctx) {
    sw_ = sw;
    sh_ = sh;
    dw_ = dw;
    dh_ = dh;
    sink_ = sink;
    ctx_ = ctx;
    srcRow_ = 0;
    dstRow_ = 0;
    accW_ = 0;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    cur_.assign(dw, 255);
    prev_.assign(dw, 255);
    out_.assign(dw, 255);
    if (dh <= sh) acc_.assign(dw, 0);
    return true;
  }

  void push(const uint8_t* src) {
    if (srcRow_ >= sh_) return;
    hscale(src, cur_.data());
    const int r = srcRow_++;
    if (dh_ <= sh_) {
      // Each output row averages the source rows it covers.
      const uint64_t rs = static_cast<uint64_t>(r) << 16;
      const uint64_t re = static_cast<uint64_t>(r + 1) << 16;
      while (dstRow_ < dh_) {
        const uint64_t ys = rowStart(dstRow_);
        const uint64_t ye = rowStart(dstRow_ + 1);
        const uint64_t lo = std::max(rs, ys);
        const uint64_t hi = std::min(re, ye);
        if (hi > lo) {
          const uint32_t w = static_cast<uint32_t>(hi - lo);
          for (int x = 0; x < dw_; x++) acc_[x] += static_cast<uint32_t>(cur_[x]) * (w >> 4);
          accW_ += w >> 4;
        }
        if (ye > re) break;
        for (int x = 0; x < dw_; x++) out_[x] = static_cast<uint8_t>(accW_ ? (acc_[x] + accW_ / 2) / accW_ : 255);
        sink_(ctx_, dstRow_++, out_.data(), dw_);
        std::fill(acc_.begin(), acc_.end(), 0);
        accW_ = 0;
      }
    } else {
      // Output rows between the previous source row and this one.
      while (dstRow_ < dh_) {
        const int64_t sy = sourceY(dstRow_);
        if (sy <= 0) {
          if (r != 0) break;
          sink_(ctx_, dstRow_++, cur_.data(), dw_);
          continue;
        }
        if (sy > (static_cast<int64_t>(r) << 16)) break;
        const uint32_t f = static_cast<uint32_t>(sy - (static_cast<int64_t>(r - 1) << 16));
        for (int x = 0; x < dw_; x++) {
          out_[x] = static_cast<uint8_t>((prev_[x] * (65536 - f) + cur_[x] * f + 32768) >> 16);
        }
        sink_(ctx_, dstRow_++, out_.data(), dw_);
      }
      prev_.swap(cur_);
    }
  }

  void finish() {
    // Rows below the last source row repeat it.
    const std::vector<uint8_t>& last = dh_ <= sh_ ? out_ : prev_;
    while (dstRow_ < dh_) sink_(ctx_, dstRow_++, last.data(), dw_);
  }

 private:
  uint64_t rowStart(int y) const { return (static_cast<uint64_t>(y) * sh_ << 16) / dh_; }
  int64_t sourceY(int y) const { return ((static_cast<int64_t>(2 * y + 1) * sh_ << 16) / (2 * dh_)) - 32768; }

  void hscale(const uint8_t* src, uint8_t* dst) const {
    if (dw_ <= sw_) {
      for (int x = 0; x < dw_; x++) {
        const uint64_t a = (static_cast<uint64_t>(x) * sw_ << 16) / dw_;
        const uint64_t b = (static_cast<uint64_t>(x + 1) * sw_ << 16) / dw_;
        uint64_t sum = 0;
        for (uint64_t i = a >> 16; i < static_cast<uint64_t>(sw_) && (i << 16) < b; i++) {
          const uint64_t lo = std::max(a, i << 16);
          const uint64_t hi = std::min(b, (i + 1) << 16);
          sum += static_cast<uint64_t>(src[i]) * (hi - lo);
        }
        dst[x] = static_cast<uint8_t>(b > a ? (sum + (b - a) / 2) / (b - a) : src[std::min<int>(sw_ - 1, a >> 16)]);
      }
    } else {
      for (int x = 0; x < dw_; x++) {
        int64_t p = ((static_cast<int64_t>(2 * x + 1) * sw_ << 16) / (2 * dw_)) - 32768;
        p = std::max<int64_t>(0, std::min<int64_t>(p, static_cast<int64_t>(sw_ - 1) << 16));
        const int i = static_cast<int>(p >> 16);
        const int j = std::min(i + 1, sw_ - 1);
        const uint32_t f = static_cast<uint32_t>(p & 0xFFFF);
        dst[x] = static_cast<uint8_t>((src[i] * (65536 - f) + src[j] * f + 32768) >> 16);
      }
    }
  }

  int sw_ = 0, sh_ = 0, dw_ = 0, dh_ = 0;
  Sink sink_ = nullptr;
  void* ctx_ = nullptr;
  int srcRow_ = 0, dstRow_ = 0;
  uint32_t accW_ = 0;
  std::vector<uint8_t> cur_, prev_, out_;
  std::vector<uint32_t> acc_;
};

// Where scaled rows go: the visible part into a pixmap, or straight onto
// the screen in black and white when no pixmap could be had.
struct RegionSink {
  int viewX = 0, viewY = 0, viewW = 0, viewH = 0;
  bool dither = true;  // photos; line art keeps its levels
  Pixmap* out = nullptr;
  GfxRenderer* direct = nullptr;
  int x = 0, y = 0;
  DirectPixelWriter pw;

  static void row(void* ctx, int ry, const uint8_t* grey, int w) {
    auto* s = static_cast<RegionSink*>(ctx);
    if (ry < s->viewY || ry >= s->viewY + s->viewH) return;
    const int py = ry - s->viewY;
    const int x0 = std::max(0, s->viewX);
    const int x1 = std::min(w, s->viewX + s->viewW);
    if (s->out) {
      for (int sx = x0; sx < x1; sx++) {
        const int px = sx - s->viewX;
        const uint8_t g = grey[sx];
        const uint8_t level = s->dither ? applyBayerDither4Level(g, px, py) : static_cast<uint8_t>((g * 3 + 127) / 255);
        if (level != 3) s->out->set(px, py, level);
      }
      return;
    }
    // One bit: ordered dither against a 4×4 threshold.
    const int sy = s->y + py;
    if (sy < 0 || sy >= s->direct->getScreenHeight()) return;
    s->pw.beginRow(sy);
    for (int sx = x0; sx < x1; sx++) {
      const int px = s->x + sx - s->viewX;
      if (px < 0 || px >= s->direct->getScreenWidth()) continue;
      const uint8_t t = static_cast<uint8_t>(bayer4x4[sy & 3][px & 3] * 16 + 8);
      if (grey[sx] < t) s->pw.writePixel(px, 0);
    }
  }
};

// ------------------------------------------------------------------ JPEG

struct JpegSource {
  WikiSession* session;
  xwa::ImageRecord rec;
};

struct JpegDecode {
  RowScaler* scaler;
  int width = 0;  // decoded (pre-scaled) size
  int height = 0;
  std::vector<uint8_t> band;
  int bandY = -1;
  int bandRows = 0;
  int nextRow = 0;
  uint32_t lastYield = 0;
};

void* jpegOpen(const char* handle, int32_t* size) {
  auto* src = const_cast<JpegSource*>(reinterpret_cast<const JpegSource*>(handle));
  *size = static_cast<int32_t>(src->rec.size);
  return src;
}

void jpegClose(void*) {}

int32_t jpegRead(JPEGFILE* f, uint8_t* buf, int32_t len) {
  auto* src = static_cast<JpegSource*>(f->fHandle);
  const int32_t left = static_cast<int32_t>(src->rec.size) - f->iPos;
  const int32_t n = std::max<int32_t>(0, std::min<int32_t>(len, left));
  if (n == 0) return 0;
  if (!src->session->archive().readImage(src->rec, static_cast<uint32_t>(f->iPos), buf, static_cast<size_t>(n))) {
    return 0;
  }
  f->iPos += n;
  return n;
}

int32_t jpegSeek(JPEGFILE* f, int32_t pos) {
  auto* src = static_cast<JpegSource*>(f->fHandle);
  if (pos < 0 || pos > static_cast<int32_t>(src->rec.size)) return -1;
  f->iPos = pos;
  return pos;
}

// Flushes complete rows of the band to the scaler.
void flushBand(JpegDecode* d) {
  for (int r = 0; r < d->bandRows && d->bandY + r < d->height; r++) {
    if (d->bandY + r != d->nextRow) continue;
    d->scaler->push(d->band.data() + static_cast<size_t>(r) * d->width);
    d->nextRow++;
  }
  d->bandY = -1;
}

int jpegDraw(JPEGDRAW* draw) {
  auto* d = static_cast<JpegDecode*>(draw->pUser);
  const uint32_t now = millis();
  if (now - d->lastYield >= 250) {
    d->lastYield = now;
    delay(1);
  }
  if (draw->y != d->bandY) {
    if (d->bandY >= 0) flushBand(d);
    d->bandY = draw->y;
    d->bandRows = draw->iHeight;
    d->band.assign(static_cast<size_t>(d->width) * std::max(1, draw->iHeight), 255);
  }
  const auto* px = reinterpret_cast<const uint8_t*>(draw->pPixels);
  const int used = std::min(draw->iWidthUsed, d->width - draw->x);
  for (int r = 0; r < draw->iHeight && r < d->bandRows; r++) {
    if (used > 0)
      std::memcpy(d->band.data() + static_cast<size_t>(r) * d->width + draw->x, px + r * draw->iWidth, used);
  }
  if (draw->x + draw->iWidthUsed >= d->width) flushBand(d);
  return 1;
}

bool decodeJpeg(WikiSession& session, const xwa::ImageRecord& rec, int sw, int sh, RegionSink& sink) {
  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) return false;
  JpegSource src{&session, rec};
  if (jpeg->open(reinterpret_cast<const char*>(&src), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDraw) != 1) {
    LOG_ERR("WIKI", "JPEG open failed");
    return false;
  }
  const int w = jpeg->getWidth();
  const int h = jpeg->getHeight();
  if (w <= 0 || h <= 0) return false;
  // JPEGDEC's own reduction (1/2, 1/4, 1/8) down to no less than the target.
  int denom = 1;
  int option = 0;
  const int scales[3][2] = {{8, JPEG_SCALE_EIGHTH}, {4, JPEG_SCALE_QUARTER}, {2, JPEG_SCALE_HALF}};
  for (const auto& s : scales) {
    if (w / s[0] >= sw && h / s[0] >= sh && jpeg->getJPEGType() == JPEG_MODE_BASELINE) {
      denom = s[0];
      option = s[1];
      break;
    }
  }
  if (jpeg->getJPEGType() != JPEG_MODE_BASELINE) {
    denom = 8;  // JPEGDEC decodes progressive files at 1/8 only
    option = JPEG_SCALE_EIGHTH;
  }
  JpegDecode d;
  d.width = (w + denom - 1) / denom;
  d.height = (h + denom - 1) / denom;
  RowScaler scaler;
  if (!scaler.begin(d.width, d.height, sw, sh, RegionSink::row, &sink)) return false;
  d.scaler = &scaler;
  d.lastYield = millis();
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&d);
  const int rc = jpeg->decode(0, 0, option);
  if (d.bandY >= 0) flushBand(&d);
  jpeg->close();
  if (rc != 1) {
    LOG_ERR("WIKI", "JPEG decode failed (%d)", jpeg->getLastError());
    return false;
  }
  scaler.finish();
  return true;
}

bool decodeLa2(WikiSession& session, const xwa::ImageRecord& rec, int sw, int sh, RegionSink& sink) {
  xwa::La2Reader la2;
  if (!la2.begin(session.archive(), rec)) return false;
  RowScaler scaler;
  if (!scaler.begin(la2.width(), la2.height(), sw, sh, RegionSink::row, &sink)) return false;
  std::vector<uint8_t> levels(la2.width());
  const uint8_t max = la2.maxLevel();
  uint8_t grey[4];
  for (int i = 0; i <= max; i++) grey[i] = static_cast<uint8_t>(255 - i * 255 / max);
  for (uint16_t y = 0; y < la2.height(); y++) {
    if (!la2.row(levels.data())) return false;
    for (auto& v : levels) v = grey[v > max ? max : v];
    scaler.push(levels.data());
  }
  scaler.finish();
  return true;
}

bool decodeRegion(WikiSession& session, uint32_t id, int sw, int sh, RegionSink& sink) {
  xwa::ImageRecord rec;
  if (!session.archive().imageRecord(id, rec)) return false;
  if (rec.encoding == xwa::image_encoding::JPEG) {
    sink.dither = true;
    return decodeJpeg(session, rec, sw, sh, sink);
  }
  if (rec.encoding == xwa::image_encoding::LINE_ART) {
    sink.dither = false;
    return decodeLa2(session, rec, sw, sh, sink);
  }
  return false;
}

}  // namespace

bool decodePictureRegion(WikiSession& session, uint32_t id, int sw, int sh, int viewX, int viewY, int viewW, int viewH,
                         Pixmap& out) {
  if (!out.allocate(viewW, viewH)) return false;
  RegionSink sink;
  sink.viewX = viewX;
  sink.viewY = viewY;
  sink.viewW = viewW;
  sink.viewH = viewH;
  sink.out = &out;
  if (!decodeRegion(session, id, sw, sh, sink)) {
    out.release();
    return false;
  }
  return true;
}

bool decodePicture(WikiSession& session, uint32_t id, int w, int h, Pixmap& out) {
  return decodePictureRegion(session, id, w, h, 0, 0, w, h, out);
}

bool drawPictureRegion(GfxRenderer& renderer, WikiSession& session, uint32_t id, int sw, int sh, int viewX, int viewY,
                       int viewW, int viewH, int x, int y) {
  RegionSink sink;
  sink.viewX = viewX;
  sink.viewY = viewY;
  sink.viewW = viewW;
  sink.viewH = viewH;
  sink.direct = &renderer;
  sink.x = x;
  sink.y = y;
  sink.pw.init(renderer);
  return decodeRegion(session, id, sw, sh, sink);
}

// ------------------------------------------------------------------ formulas

namespace {
void coverage(void* ctx, int y, int x, const uint8_t* cov, int n) {
  auto* p = static_cast<Pixmap*>(ctx);
  if (y < 0 || y >= p->height()) return;
  for (int i = 0; i < n; i++) {
    const int px = x + i;
    if (px < 0 || px >= p->width() || cov[i] == 0) continue;
    // Darken: overlapping items keep the darker level.
    const uint8_t level = static_cast<uint8_t>(3 - (cov[i] * 3 + 127) / 255);
    if (level < p->get(px, y)) p->set(px, y, level);
  }
}
}  // namespace

bool renderFormula(WikiSession& session, uint32_t article, uint32_t pos, uint32_t len, int w, int h, Pixmap& out) {
  xwa::StreamReader r;
  std::vector<uint8_t> buf;
  xwa::Formula f;
  if (!r.open(session.archive(), article) || !xwa::loadFormula(r, pos, len, buf, f)) return false;
  xwa::GlyphTable none;
  xwa::GlyphTable* glyphs = session.glyphs();
  if (!xwa::drawable(f)) {
    return f.hasFallback && decodePicture(session, f.fallbackImage, w, h, out);
  }
  if (!out.allocate(w, h)) return false;
  const float ppu = static_cast<float>(w) / std::max<int32_t>(1, f.width);
  const float baseline = std::max<int32_t>(0, f.ascent) * ppu;
  if (!xwa::drawFormula(f, glyphs ? *glyphs : none, 0.0f, baseline, ppu, coverage, &out) && f.hasFallback) {
    // Missing glyphs: the picture is better than a formula with holes.
    Pixmap pic;
    if (decodePicture(session, f.fallbackImage, w, h, pic)) out = std::move(pic);
  }
  return true;
}

}  // namespace wiki
