#include "desktop/panel_view.h"

#include <algorithm>

namespace bn::hw {

RefreshStage StageAt(const SimDisplay::Refresh& refresh, std::uint64_t nowUs) noexcept {
  if (nowUs >= refresh.doneUs) {
    return RefreshStage::kAfter;
  }
  const std::uint64_t length = refresh.doneUs - refresh.startUs;
  const std::uint64_t into = nowUs > refresh.startUs ? nowUs - refresh.startUs : 0;
  switch (refresh.mode) {
    case core::RefreshMode::kFast:
      return RefreshStage::kBefore;
    case core::RefreshMode::kHalf:
      return into * 2 < length ? RefreshStage::kBlack : RefreshStage::kWhite;
    case core::RefreshMode::kFull:
      break;
  }
  if (into * 3 < length) {
    return RefreshStage::kInverted;
  }
  return into * 3 < length * 2 ? RefreshStage::kBlack : RefreshStage::kWhite;
}

PanelView::PanelView(core::DisplaySize panel, ViewTurn turn)
    : panel_(panel),
      turn_(turn),
      pixels_(static_cast<std::size_t>(panel.width) * panel.height, kWhite),
      paintedTurn_(turn) {}

core::DisplaySize PanelView::Size() const noexcept {
  if (turn_ == ViewTurn::kAsRead) {
    return core::DisplaySize{.width = panel_.height, .height = panel_.width};
  }
  return panel_;
}

bool PanelView::Update(const SimDisplay& display, std::uint64_t nowUs) {
  const RefreshStage stage = StageAt(display.LastRefresh(), nowUs);
  if (painted_ && stage == stage_ && display.RefreshCount() == refreshCount_ && turn_ == paintedTurn_) {
    return false;
  }
  switch (stage) {
    case RefreshStage::kBefore:
      Paint(display.ShownBefore(), false);
      break;
    case RefreshStage::kInverted:
      Paint(display.ShownBefore(), true);
      break;
    case RefreshStage::kBlack:
      Fill(kBlack);
      break;
    case RefreshStage::kWhite:
      Fill(kWhite);
      break;
    case RefreshStage::kAfter:
      Paint(display.Shown(), false);
      break;
  }
  painted_ = true;
  stage_ = stage;
  refreshCount_ = display.RefreshCount();
  paintedTurn_ = turn_;
  return true;
}

void PanelView::Paint(std::span<const std::uint8_t> rows, bool inverted) {
  const std::size_t rowBytes = panel_.RowBytes();
  const std::size_t width = panel_.width;
  const std::size_t height = panel_.height;
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const unsigned byte = rows[(y * rowBytes) + (x / 8)];
      const bool white = ((byte >> (7 - (x % 8))) & 1U) != 0;
      const std::uint32_t pixel = white != inverted ? kWhite : kBlack;
      // Turned a quarter-turn clockwise, a pixel's row becomes its column
      // counted from the right, and its column its row.
      const std::size_t at = turn_ == ViewTurn::kAsRead ? (x * height) + (height - 1 - y) : (y * width) + x;
      pixels_[at] = pixel;
    }
  }
}

void PanelView::Fill(std::uint32_t pixel) { std::fill(pixels_.begin(), pixels_.end(), pixel); }

}  // namespace bn::hw
