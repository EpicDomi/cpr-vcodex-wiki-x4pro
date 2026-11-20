#include "WikiTableActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int PAD = 5;
constexpr size_t MAX_CELLS = 3000;
}  // namespace

WikiTableActivity::WikiTableActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     std::shared_ptr<WikiSession> session, uint32_t article, uint32_t table)
    : Activity("WikiTable", renderer, mappedInput), session_(std::move(session)), article_(article), table_(table) {}

void WikiTableActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  viewX_ = content.x + 4;
  viewW_ = content.width - 8;
  viewY_ = metrics.topPadding + metrics.headerHeight + 4;
  viewH_ = renderer.getScreenHeight() - viewY_ - metrics.buttonHintsHeight - 4;
  {
    RenderLock lock(*this);
    failed_ = !build();
  }
  requestUpdate();
}

void WikiTableActivity::onExit() {
  pixmaps_.clear();
  layout_.reset();
  Activity::onExit();
}

bool WikiTableActivity::build() {
  xwa::StreamReader r;
  if (!r.open(session_->archive(), article_) || !xwa::readHeader(r, header_)) return false;
  xwa::TableInfo t;
  if (!xwa::readTable(r, header_, table_, t, MAX_CELLS)) return false;
  style_ = wiki::TextStyle::fromSettings(renderer);
  layout_.reset(new (std::nothrow) wiki::Layout(renderer, *session_, article_, header_, style_,
                                                static_cast<int16_t>(viewW_), static_cast<int16_t>(viewH_)));
  if (!layout_ || t.cols == 0 || t.cells.empty()) return false;
  const uint32_t rows = std::max<uint32_t>(t.rows, t.cells.back().row + 1u);
  const uint32_t cols = t.cols;

  // The caption, bold, above the table (spec 7.4).
  wiki::BlockLines caption;
  int captionH = 0;
  if (t.captionLen > 0) {
    layout_->flowRun(t.captionPos, t.captionLen, wiki::FONT_SMALL, EpdFontFamily::BOLD,
                     static_cast<int16_t>(std::max(8, viewW_ - 2 * PAD)), 1, caption);
    for (const auto& ln : caption.lines) captionH += ln.h;
    if (captionH > 0) captionH += 2 * PAD;
  }

  // Natural widths: each cell on one line.
  const int maxCol = std::max(140, viewW_ * 3 / 5);
  const int minCol = 48;
  std::vector<int> colW(cols, minCol);
  std::vector<int> natural(t.cells.size(), 0);
  for (size_t i = 0; i < t.cells.size(); i++) {
    const auto& c = t.cells[i];
    wiki::BlockLines one;
    layout_->flowRun(c.runPos, c.runLen, wiki::FONT_SMALL, c.header ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR,
                     4000, 1, one);
    int w = 0;
    for (const auto& it : one.items) w = std::max(w, it.x + it.w);
    natural[i] = w + 2 * PAD;
    if (c.colspan == 1 && c.col < cols) colW[c.col] = std::max(colW[c.col], std::min(maxCol, natural[i]));
  }
  // Spanning cells widen the columns they cover when those are too narrow.
  for (size_t i = 0; i < t.cells.size(); i++) {
    const auto& c = t.cells[i];
    if (c.colspan <= 1 || c.col >= cols) continue;
    const uint32_t end = std::min<uint32_t>(cols, c.col + c.colspan);
    int have = 0;
    for (uint32_t k = c.col; k < end; k++) have += colW[k];
    const int want = std::min(natural[i], maxCol * static_cast<int>(end - c.col));
    if (want > have) {
      const int extra = (want - have + static_cast<int>(end - c.col) - 1) / static_cast<int>(end - c.col);
      for (uint32_t k = c.col; k < end; k++) colW[k] += extra;
    }
  }
  int total = 0;
  for (int w : colW) total += w;
  if (total < viewW_) {
    // A narrow table spreads across the screen.
    const int extra = viewW_ - total;
    for (uint32_t k = 0; k < cols; k++) colW[k] += extra * colW[k] / total;
  }
  colLefts_.assign(cols + 1, 0);
  for (uint32_t k = 0; k < cols; k++) colLefts_[k + 1] = static_cast<int16_t>(colLefts_[k] + colW[k]);
  tableW_ = colLefts_[cols];

  // Lay out each cell at its width; rows grow to their tallest cell.
  std::vector<wiki::BlockLines> laid(t.cells.size());
  std::vector<int> rowH(rows, 0);
  for (size_t i = 0; i < t.cells.size(); i++) {
    const auto& c = t.cells[i];
    if (c.col >= cols) continue;
    const uint32_t end = std::min<uint32_t>(cols, c.col + c.colspan);
    const int w = colLefts_[end] - colLefts_[c.col] - 2 * PAD;
    layout_->flowRun(c.runPos, c.runLen, wiki::FONT_SMALL, c.header ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR,
                     static_cast<int16_t>(std::max(8, w)), c.header ? 2 : 1, laid[i]);
    int h = 2 * PAD;
    for (const auto& ln : laid[i].lines) h += ln.h;
    if (c.rowspan == 1 && c.row < rows) rowH[c.row] = std::max(rowH[c.row], h);
  }
  for (size_t i = 0; i < t.cells.size(); i++) {
    const auto& c = t.cells[i];
    if (c.rowspan <= 1 || c.row >= rows) continue;
    const uint32_t end = std::min<uint32_t>(rows, c.row + c.rowspan);
    int have = 0;
    for (uint32_t k = c.row; k < end; k++) have += rowH[k];
    int need = 2 * PAD;
    for (const auto& ln : laid[i].lines) need += ln.h;
    if (need > have) rowH[end - 1] += need - have;
  }
  rowTops_.assign(rows + 1, 0);
  rowTops_[0] = static_cast<int16_t>(std::min(30000, captionH));
  for (uint32_t k = 0; k < rows; k++) {
    const int h = std::max(rowH[k], static_cast<int>(style_.lineHeight[wiki::FONT_SMALL]) + 2 * PAD);
    rowTops_[k + 1] = static_cast<int16_t>(std::min(30000, rowTops_[k] + h));
  }
  tableH_ = rowTops_[rows];

  // Flatten the cells into table coordinates.
  cells_.clear();
  items_.clear();
  areas_.clear();
  text_.clear();
  // Moves a cell's lines into table coordinates.
  auto place = [&](Cell& cell, const wiki::BlockLines& lines) {
    cell.firstItem = static_cast<uint32_t>(items_.size());
    cell.firstArea = static_cast<uint32_t>(areas_.size());
    int y = cell.y + PAD;
    for (const auto& ln : lines.lines) {
      for (uint16_t k = 0; k < ln.items; k++) {
        wiki::DrawItem it = lines.items[ln.item + k];
        it.x = static_cast<int16_t>(it.x + cell.x + PAD);
        it.y = static_cast<int16_t>(it.y + y);
        if (it.hasText()) {
          const uint32_t from = it.a;
          it.a = static_cast<uint32_t>(text_.size());
          text_.append(lines.text.c_str() + from, it.b);
          text_.push_back('\0');
        }
        items_.push_back(it);
      }
      for (uint16_t k = 0; k < ln.areas; k++) {
        wiki::HitArea a = lines.areas[ln.area + k];
        a.x = static_cast<int16_t>(a.x + cell.x + PAD);
        a.y = static_cast<int16_t>(a.y + y);
        areas_.push_back(a);
      }
      y += ln.h;
    }
    cell.items = static_cast<uint32_t>(items_.size()) - cell.firstItem;
    cell.areas = static_cast<uint32_t>(areas_.size()) - cell.firstArea;
    cells_.push_back(cell);
  };
  if (captionH > 0) {
    Cell cell;
    cell.w = static_cast<int16_t>(viewW_);
    cell.h = static_cast<int16_t>(captionH);
    cell.frame = false;
    place(cell, caption);
  }
  for (size_t i = 0; i < t.cells.size(); i++) {
    const auto& c = t.cells[i];
    if (c.col >= cols || c.row >= rows) continue;
    Cell cell;
    cell.x = colLefts_[c.col];
    cell.y = rowTops_[c.row];
    cell.w = static_cast<int16_t>(colLefts_[std::min<uint32_t>(cols, c.col + c.colspan)] - cell.x);
    cell.h = static_cast<int16_t>(rowTops_[std::min<uint32_t>(rows, c.row + c.rowspan)] - cell.y);
    cell.header = c.header;
    place(cell, laid[i]);
    laid[i] = wiki::BlockLines();  // free as we go
  }
  return true;
}

void WikiTableActivity::scroll(int dx, int dy) {
  const int maxX = std::max(0, tableW_ - viewW_);
  const int maxY = std::max(0, tableH_ - viewH_);
  if (dy > 0) {
    if (scrollY_ < maxY) {
      // Down a screenful, starting with the row that was cut off.
      int next = scrollY_ + viewH_;
      for (size_t k = 1; k < rowTops_.size(); k++) {
        if (rowTops_[k] > scrollY_ + viewH_) {
          next = rowTops_[k - 1] > scrollY_ ? rowTops_[k - 1] : scrollY_ + viewH_;
          break;
        }
      }
      scrollY_ = std::min(maxY, next);
    } else if (scrollX_ < maxX) {
      // At the bottom: the next band of columns, from the top.
      scrollX_ = std::min(maxX, scrollX_ + viewW_ - 40);
      scrollY_ = 0;
    }
  } else if (dy < 0) {
    if (scrollY_ > 0) {
      scrollY_ = std::max(0, scrollY_ - viewH_);
    } else if (scrollX_ > 0) {
      scrollX_ = std::max(0, scrollX_ - (viewW_ - 40));
      scrollY_ = maxY;
    }
  }
  if (dx != 0) scrollX_ = std::max(0, std::min(maxX, scrollX_ + dx * (viewW_ - 40)));
}

void WikiTableActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
    return;
  }
  int tx = 0;
  int ty = 0;
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(tx, ty)) {
    const int x = tx - viewX_ + scrollX_;
    const int y = ty - viewY_ + scrollY_;
    for (const auto& a : areas_) {
      if (a.kind == wiki::HitArea::Link && x >= a.x - 4 && x < a.x + a.w + 4 && y >= a.y && y < a.y + a.h) {
        setResult(WikiLinkResult{a.target, a.place, a.anchored});
        finish();
        return;
      }
    }
  }
  const auto swipe = mappedInput.wasSwipe();
  int dx = 0;
  int dy = 0;
  if (swipe == MappedInputManager::SwipeDir::Up) dy = 1;
  if (swipe == MappedInputManager::SwipeDir::Down) dy = -1;
  if (swipe == MappedInputManager::SwipeDir::Left) dx = 1;
  if (swipe == MappedInputManager::SwipeDir::Right && !mappedInput.wasBackGesture()) dx = -1;
  const auto turn = ReaderUtils::detectPageTurn(mappedInput);
  if (turn.next) dy = 1;
  if (turn.prev) dy = -1;
  if (dx == 0 && dy == 0) return;
  const int oldX = scrollX_;
  const int oldY = scrollY_;
  scroll(dx, dy);
  if (scrollX_ != oldX || scrollY_ != oldY) requestUpdate();
}

void WikiTableActivity::draw() {
  const auto clip = renderer.getClipRect();
  renderer.setClipRect(viewX_, viewY_, viewW_, viewH_);
  const int ox = viewX_ - scrollX_;
  const int oy = viewY_ - scrollY_;
  for (const Cell& c : cells_) {
    if (c.x + c.w < scrollX_ || c.x > scrollX_ + viewW_ || c.y + c.h < scrollY_ || c.y > scrollY_ + viewH_) continue;
    if (c.frame) renderer.drawRect(ox + c.x, oy + c.y, c.w + 1, c.h + 1, true);
    for (uint32_t k = c.firstItem; k < c.firstItem + c.items; k++) {
      const wiki::DrawItem& it = items_[k];
      switch (it.kind) {
        case wiki::DrawItem::Text:
          renderer.drawText(style_.id[it.font], ox + it.x, oy + it.y, text_.c_str() + it.a, true,
                            static_cast<EpdFontFamily::Style>(it.style), BidiUtils::BidiBaseDir::AUTO, style_.tracking);
          break;
        case wiki::DrawItem::Fill:
          renderer.fillRect(ox + it.x, oy + it.y, it.w, it.h, true);
          break;
        case wiki::DrawItem::Frame:
          renderer.drawRect(ox + it.x, oy + it.y, it.w, it.h, true);
          break;
        case wiki::DrawItem::Glyphs:
          // Drawn afresh each pass: a table's cells may hold far more than a
          // page, and only the visible ones are drawn.
          wiki::drawGlyphRun(renderer, *session_, text_.c_str() + it.a, it.b, style_.em[it.font],
                             style_.ascent[it.font], it.style, style_.tracking, ox + it.x, oy + it.y, it.w, it.h);
          break;
        default: {
          auto found = pixmaps_.find(k);
          if (found == pixmaps_.end()) {
            wiki::Pixmap p;
            if (it.kind == wiki::DrawItem::Image) {
              wiki::decodePicture(*session_, it.a, it.w, it.h, p);
            } else {
              wiki::renderFormula(*session_, article_, it.a, it.b, it.w, it.h, p);
            }
            found = pixmaps_.emplace(k, std::move(p)).first;
          }
          if (found->second.valid()) found->second.draw(renderer, ox + it.x, oy + it.y);
          break;
        }
      }
    }
  }
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
}

void WikiTableActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  char title[48];
  snprintf(title, sizeof(title), "%s %u", tr(STR_WIKI_TABLE), static_cast<unsigned>(table_ + 1));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
  if (failed_) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_WIKI_ARTICLE_DAMAGED));
  } else {
    draw();
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.textAntiAliasing && !renderer.isDarkMode() && !failed_) {
    renderer.displayBuffer();
    ReaderUtils::renderAntiAliased(renderer, [this]() { draw(); });
  } else {
    renderer.displayBuffer();
  }
}
