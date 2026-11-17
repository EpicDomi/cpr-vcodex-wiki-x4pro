#include "XwaMath.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace xwa {

// ------------------------------------------------------------------ glyphs

bool GlyphTable::open(Archive& archive) {
  ar_ = nullptr;
  const SectionInfo& s = archive.sectionInfo(section::MATH_GLYPHS);
  if (!s.present() || s.length < 5) return false;
  uint8_t head[5];
  if (!archive.readCached(s.offset, head, sizeof(head))) return false;
  upem_ = le16(head);
  count_ = le16(head + 2);
  const uint8_t scaleCount = head[4];
  scales_.resize(scaleCount);
  offsetsAt_ = 5ull + 2ull * scaleCount;
  dataAt_ = offsetsAt_ + 4ull * (count_ + 1);
  if (upem_ == 0 || dataAt_ > s.length) return false;
  for (uint8_t i = 0; i < scaleCount; i++) {
    uint8_t v[2];
    if (!archive.readCached(s.offset + 5 + 2 * i, v, 2)) return false;
    scales_[i] = le16(v);
  }
  base_ = s.offset;
  length_ = s.length;
  ar_ = &archive;
  return true;
}

bool GlyphTable::glyph(uint32_t index, std::vector<uint8_t>& out) {
  if (!ar_ || index >= count_) return false;
  uint8_t o[8];
  if (!ar_->readCached(base_ + offsetsAt_ + 4ull * index, o, 8)) return false;
  const uint32_t a = le32(o);
  const uint32_t b = le32(o + 4);
  if (b < a || b - a < 9 || dataAt_ + b > length_ || b - a > 65536) return false;
  out.resize(b - a);
  return ar_->readCached(base_ + dataAt_ + a, out.data(), out.size());
}

bool outlineGlyph(const uint8_t* data, size_t len, Rasterizer& r, float ox, float oy, float sx, float sy) {
  if (len < 8) return false;
  const uint8_t* p = data + 8;  // bbox
  const uint8_t* end = data + len;
  auto pt = [&](int i, float& x, float& y) {
    x = ox + static_cast<int16_t>(le16(p + 4 * i)) * sx;
    y = oy - static_cast<int16_t>(le16(p + 4 * i + 2)) * sy;
  };
  static const uint8_t points[4] = {1, 1, 2, 3};
  while (p < end) {
    const uint8_t cmd = *p++;
    if (cmd == 5) return true;
    if (cmd == 4) {
      r.close();
      continue;
    }
    if (cmd > 3 || end - p < 4 * points[cmd]) return false;
    float x[3];
    float y[3];
    for (int i = 0; i < points[cmd]; i++) pt(i, x[i], y[i]);
    p += 4 * points[cmd];
    switch (cmd) {
      case 0:
        r.moveTo(x[0], y[0]);
        break;
      case 1:
        r.lineTo(x[0], y[0]);
        break;
      case 2:
        r.quadTo(x[0], y[0], x[1], y[1]);
        break;
      default:
        r.cubicTo(x[0], y[0], x[1], y[1], x[2], y[2]);
    }
  }
  return false;
}

// ------------------------------------------------------------------ raster

bool Rasterizer::begin(int x0, int y0, int w, int h) {
  x0_ = x0;
  y0_ = y0;
  w_ = std::max(0, w);
  h_ = std::max(0, h);
  lines_.clear();
  open_ = false;
  return w_ > 0 && h_ > 0;
}

void Rasterizer::add(float ax, float ay, float bx, float by) {
  if (ay == by) return;
  ax -= x0_;
  bx -= x0_;
  ay -= y0_;
  by -= y0_;
  // Split where the line crosses the box's left or right side, so every
  // piece lies wholly inside or wholly outside: fill() can then clamp x
  // without changing any pixel's coverage.
  float t[2];
  float side[2];
  int n = 0;
  for (const float s : {0.0f, static_cast<float>(w_)}) {
    if ((ax < s && bx > s) || (ax > s && bx < s)) {
      t[n] = (s - ax) / (bx - ax);
      side[n++] = s;
    }
  }
  if (n == 2 && t[0] > t[1]) {
    std::swap(t[0], t[1]);
    std::swap(side[0], side[1]);
  }
  float px = ax;
  float py = ay;
  for (int i = 0; i < n; i++) {
    const float y = ay + (by - ay) * t[i];
    if (y != py) lines_.push_back(Line{px, py, side[i], y});
    px = side[i];
    py = y;
  }
  if (by != py) lines_.push_back(Line{px, py, bx, by});
}

void Rasterizer::moveTo(float x, float y) {
  close();
  startX_ = curX_ = x;
  startY_ = curY_ = y;
  open_ = true;
}

void Rasterizer::lineTo(float x, float y) {
  add(curX_, curY_, x, y);
  curX_ = x;
  curY_ = y;
}

void Rasterizer::quadTo(float cx, float cy, float x, float y) {
  const float dx = curX_ - 2 * cx + x;
  const float dy = curY_ - 2 * cy + y;
  const float dd = std::sqrt(dx * dx + dy * dy);
  const int n = std::max(1, std::min(32, static_cast<int>(std::sqrt(dd * 2.0f)) + 1));
  const float sx = curX_;
  const float sy = curY_;
  for (int i = 1; i <= n; i++) {
    const float t = static_cast<float>(i) / n;
    const float u = 1 - t;
    lineTo(u * u * sx + 2 * u * t * cx + t * t * x, u * u * sy + 2 * u * t * cy + t * t * y);
  }
}

void Rasterizer::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
  const float ax = curX_ - 2 * c1x + c2x;
  const float ay = curY_ - 2 * c1y + c2y;
  const float bx = c1x - 2 * c2x + x;
  const float by = c1y - 2 * c2y + y;
  const float dd = std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));
  const int n = std::max(1, std::min(48, static_cast<int>(std::sqrt(dd * 3.0f)) + 1));
  const float sx = curX_;
  const float sy = curY_;
  for (int i = 1; i <= n; i++) {
    const float t = static_cast<float>(i) / n;
    const float u = 1 - t;
    lineTo(u * u * u * sx + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * x,
           u * u * u * sy + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * y);
  }
}

void Rasterizer::close() {
  if (open_ && (curX_ != startX_ || curY_ != startY_)) add(curX_, curY_, startX_, startY_);
  curX_ = startX_;
  curY_ = startY_;
  open_ = false;
}

void Rasterizer::fill(RowFn fn, void* ctx) {
  close();
  if (lines_.empty() || w_ <= 0 || h_ <= 0) return;
  // Signed-area accumulation (as in font-rs): each edge adds its covered
  // area to the cells it crosses; a running sum along a row gives coverage.
  // Rows are done in bands so the buffer stays small (8 KB) on the X4.
  const int stride = w_ + 2;
  const int band = std::max(1, std::min(h_, BAND_FLOATS / stride));
  acc_.resize(static_cast<size_t>(stride) * band);
  cov_.resize(w_);
  if (acc_.size() < static_cast<size_t>(stride) * band || cov_.size() < static_cast<size_t>(w_)) return;
  float* acc = acc_.data();
  uint8_t* cov = cov_.data();
  for (int top = 0; top < h_; top += band) {
    const int rows = std::min(band, h_ - top);
    std::fill(acc, acc + static_cast<size_t>(stride) * rows, 0.0f);
    bool any = false;
    for (const Line& l : lines_) {
      float ax = l.x0, ay = l.y0 - top, bx = l.x1, by = l.y1 - top;
      float dir = 1.0f;
      if (ay > by) {
        std::swap(ax, bx);
        std::swap(ay, by);
        dir = -1.0f;
      }
      if (by <= 0 || ay >= rows) continue;
      any = true;
      const float dxdy = (bx - ax) / (by - ay);
      float x = ax;
      if (ay < 0) {
        x -= ay * dxdy;
        ay = 0;
      }
      const int yEnd = std::min(rows, static_cast<int>(std::ceil(by)));
      for (int y = static_cast<int>(ay); y < yEnd; y++) {
        float* row = acc + static_cast<size_t>(y) * stride;
        const float dy = std::min(static_cast<float>(y + 1), by) - std::max(static_cast<float>(y), ay);
        const float xnext = x + dxdy * dy;
        const float d = dy * dir;
        // Pieces left of the box add their whole winding at column 0;
        // pieces right of it only reach the two spare columns.
        const float lo = std::max(0.0f, std::min(std::min(x, xnext), static_cast<float>(w_)));
        const float hi = std::max(0.0f, std::min(std::max(x, xnext), static_cast<float>(w_)));
        const float loF = std::floor(lo);
        const int loI = static_cast<int>(loF);
        const float hiC = std::ceil(hi);
        const int hiI = static_cast<int>(hiC);
        if (hiI <= loI + 1) {
          const float xm = 0.5f * (lo + hi) - loF;
          row[loI] += d - d * xm;
          row[loI + 1] += d * xm;
        } else {
          const float s = 1.0f / (hi - lo);
          const float loFrac = lo - loF;
          const float a0 = 0.5f * s * (1 - loFrac) * (1 - loFrac);
          const float hiFrac = hi - hiC + 1;
          const float am = 0.5f * s * hiFrac * hiFrac;
          row[loI] += d * a0;
          if (hiI == loI + 2) {
            row[loI + 1] += d * (1 - a0 - am);
          } else {
            const float a1 = s * (1.5f - loFrac);
            row[loI + 1] += d * (a1 - a0);
            for (int xi = loI + 2; xi < hiI - 1; xi++) row[xi] += d * s;
            const float a2 = a1 + (hiI - loI - 3) * s;
            row[hiI - 1] += d * (1 - a2 - am);
          }
          row[hiI] += d * am;
        }
        x = xnext;
      }
    }
    if (!any) continue;
    for (int y = 0; y < rows; y++) {
      const float* row = acc + static_cast<size_t>(y) * stride;
      float sum = 0;
      int first = -1;
      int last = -1;
      for (int x = 0; x < w_; x++) {
        sum += row[x];
        const float a = std::min(1.0f, std::fabs(sum));
        const uint8_t c = static_cast<uint8_t>(a * 255.0f + 0.5f);
        cov[x] = c;
        if (c) {
          if (first < 0) first = x;
          last = x;
        }
      }
      if (first >= 0) fn(ctx, y0_ + top + y, x0_ + first, cov + first, last - first + 1);
    }
  }
}

// ------------------------------------------------------------------ formulas

namespace {
// The pixel box of a rectangle given by two corners, one pixel wider all
// round for anti-aliasing.
void pixelBox(float ax, float ay, float bx, float by, int& x, int& y, int& w, int& h) {
  const float x0 = std::min(ax, bx);
  const float x1 = std::max(ax, bx);
  const float y0 = std::min(ay, by);
  const float y1 = std::max(ay, by);
  x = static_cast<int>(std::floor(x0)) - 1;
  y = static_cast<int>(std::floor(y0)) - 1;
  w = static_cast<int>(std::ceil(x1)) + 1 - x;
  h = static_cast<int>(std::ceil(y1)) + 1 - y;
}
// Largest box one item may fill: anything bigger is damage, not math.
constexpr int MAX_ITEM_PX = 4096;
}  // namespace

bool drawFormula(const Formula& f, GlyphTable& glyphs, float originX, float baselineY, float pxPerUnit,
                 Rasterizer::RowFn fn, void* ctx) {
  // Each item is filled on its own, inside its own box, so the band buffer
  // and the edge list only ever hold one glyph or shape.
  Rasterizer r;
  std::vector<uint8_t> outline;
  MathItems items(f);
  MathItem it;
  bool ok = true;
  int bx = 0, by = 0, bw = 0, bh = 0;
  while (items.next(it)) {
    const float px = originX + it.x * pxPerUnit;
    const float py = baselineY - it.y * pxPerUnit;
    switch (it.kind) {
      case MathItem::Glyph:
      case MathItem::GlyphXY: {
        float sx = 1.0f;
        float sy = 1.0f;
        if (it.kind == MathItem::Glyph) {
          if (it.scale >= glyphs.scales().size()) {
            ok = false;
            continue;
          }
          sx = sy = glyphs.scales()[it.scale] / 256.0f;
        } else {
          sx = it.sx / 256.0f;
          sy = it.sy / 256.0f;
        }
        if (!glyphs.glyph(it.glyph, outline)) {
          ok = false;
          continue;
        }
        // Formula units are MATH_GLYPHS font units, so outlines scale only
        // by the item's own factor.
        const float ux = sx * pxPerUnit;
        const float uy = sy * pxPerUnit;
        const uint8_t* bb = outline.data();
        pixelBox(px + static_cast<int16_t>(le16(bb)) * ux, py - static_cast<int16_t>(le16(bb + 2)) * uy,
                 px + static_cast<int16_t>(le16(bb + 4)) * ux, py - static_cast<int16_t>(le16(bb + 6)) * uy, bx, by, bw,
                 bh);
        if (bw > MAX_ITEM_PX || bh > MAX_ITEM_PX) {
          ok = false;
          continue;
        }
        if (!r.begin(bx, by, bw, bh)) continue;
        if (!outlineGlyph(outline.data(), outline.size(), r, px, py, ux, uy)) {
          ok = false;
          continue;
        }
        r.fill(fn, ctx);
        break;
      }
      case MathItem::Rule: {
        // Rules (fraction bars, overlines) snap to whole pixel rows, at
        // least one: a 0.6 px bar would print as a faint grey line.
        const float w = it.w * pxPerUnit;
        const float bottom = std::round(py);
        const float thick = std::max(1.0f, std::round(it.h * pxPerUnit));
        pixelBox(px, bottom - thick, px + w, bottom, bx, by, bw, bh);
        if (bw > MAX_ITEM_PX || bh > MAX_ITEM_PX) {
          ok = false;
          continue;
        }
        if (!r.begin(bx, by, bw, bh)) continue;
        r.moveTo(px, bottom - thick);
        r.lineTo(px + w, bottom - thick);
        r.lineTo(px + w, bottom);
        r.lineTo(px, bottom);
        r.close();
        r.fill(fn, ctx);
        break;
      }
      case MathItem::Path: {
        // The path's own box first, from its points.
        PathCommands scan(it);
        PathCommand c;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool any = false;
        while (scan.next(c)) {
          static const uint8_t points[5] = {1, 1, 2, 3, 0};
          for (int i = 0; i < points[c.op]; i++) {
            const float x = static_cast<float>(c.pt[2 * i]);
            const float y = static_cast<float>(c.pt[2 * i + 1]);
            if (!any) {
              minX = maxX = x;
              minY = maxY = y;
              any = true;
            }
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
          }
        }
        if (scan.bad()) {
          ok = false;
          continue;
        }
        if (!any) continue;
        pixelBox(px + minX * pxPerUnit, py - maxY * pxPerUnit, px + maxX * pxPerUnit, py - minY * pxPerUnit, bx, by, bw,
                 bh);
        if (bw > MAX_ITEM_PX || bh > MAX_ITEM_PX) {
          ok = false;
          continue;
        }
        if (!r.begin(bx, by, bw, bh)) continue;
        PathCommands cmds(it);
        while (cmds.next(c)) {
          auto X = [&](int i) { return px + c.pt[2 * i] * pxPerUnit; };
          auto Y = [&](int i) { return py - c.pt[2 * i + 1] * pxPerUnit; };
          switch (c.op) {
            case 0:
              r.moveTo(X(0), Y(0));
              break;
            case 1:
              r.lineTo(X(0), Y(0));
              break;
            case 2:
              r.quadTo(X(0), Y(0), X(1), Y(1));
              break;
            case 3:
              r.cubicTo(X(0), Y(0), X(1), Y(1), X(2), Y(2));
              break;
            default:
              r.close();
          }
        }
        r.fill(fn, ctx);
        break;
      }
    }
  }
  return ok && !items.unknown();
}

bool drawable(const Formula& f) {
  MathItems items(f);
  MathItem it;
  while (items.next(it)) {
    if (it.kind == MathItem::Path) {
      PathCommands cmds(it);
      PathCommand c;
      while (cmds.next(c)) {
      }
      if (cmds.bad()) return false;
    }
  }
  return !items.unknown();
}

}  // namespace xwa
