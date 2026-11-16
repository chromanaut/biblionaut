// The desktop's window, made with SDL (third_party/sdl). Built only when
// BN_DESKTOP_WINDOW is on.
#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

#include "desktop/device_layout.h"
#include "desktop/panel_view.h"
#include "desktop/panel_window.h"
#include "desktop/window_input.h"

namespace bn::hw {

namespace {

constexpr std::array<std::pair<SDL_Keycode, core::Button>, 10> kKeys = {{
    {SDLK_LEFT, core::Button::kLeft},
    {SDLK_RIGHT, core::Button::kRight},
    {SDLK_UP, core::Button::kUp},
    {SDLK_DOWN, core::Button::kDown},
    {SDLK_PAGEUP, core::Button::kUp},
    {SDLK_PAGEDOWN, core::Button::kDown},
    {SDLK_RETURN, core::Button::kConfirm},
    {SDLK_ESCAPE, core::Button::kBack},
    {SDLK_BACKSPACE, core::Button::kBack},
    {SDLK_P, core::Button::kPower},
}};

std::optional<core::Button> ButtonFor(SDL_Keycode key) {
  for (const auto& [code, button] : kKeys) {
    if (code == key) {
      return button;
    }
  }
  return std::nullopt;
}

struct Colour {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

constexpr Colour kOutside{.r = 0x20, .g = 0x20, .b = 0x20};  // Around the device, when the window is another shape.
constexpr Colour kBody{.r = 0x3C, .g = 0x3C, .b = 0x3E};
constexpr Colour kButton{.r = 0x6A, .g = 0x6A, .b = 0x6E};
constexpr Colour kButtonDown{.r = 0xE4, .g = 0xE4, .b = 0xE4};
constexpr Colour kLabel{.r = 0xF0, .g = 0xF0, .b = 0xF0};
constexpr Colour kLabelDown{.r = 0x20, .g = 0x20, .b = 0x20};

// SDL dates an event by its own clock, in nanoseconds since it started; the
// application's clock is another. An event `ticksNs - eventNs` before the
// moment both were read was that long before `nowUs`.
std::uint64_t OnClock(std::uint64_t eventNs, std::uint64_t ticksNs, std::uint64_t nowUs) {
  const std::uint64_t agoUs = ticksNs > eventNs ? (ticksNs - eventNs) / 1000 : 0;
  return nowUs > agoUs ? nowUs - agoUs : 0;
}

const char* TitleFor(RefreshStage stage, core::RefreshMode mode) {
  if (stage == RefreshStage::kAfter) {
    return "Biblionaut";
  }
  switch (mode) {
    case core::RefreshMode::kFast:
      return "Biblionaut (fast refresh)";
    case core::RefreshMode::kHalf:
      return "Biblionaut (half refresh)";
    case core::RefreshMode::kFull:
      break;
  }
  return "Biblionaut (full refresh)";
}

SDL_FRect ToFRect(const Rect& rect) {
  return SDL_FRect{.x = static_cast<float>(rect.x),
                   .y = static_cast<float>(rect.y),
                   .w = static_cast<float>(rect.width),
                   .h = static_cast<float>(rect.height)};
}

class SdlWindow final : public PanelWindow {
 public:
  SdlWindow(core::Clock& clock, SimDisplay& display, core::Input& script)
      : clock_(clock),
        display_(display),
        input_(clock, script),
        view_(display.Size()),
        layout_(display.Size(), view_.Turn()) {}

  ~SdlWindow() override {
    if (texture_ != nullptr) {
      SDL_DestroyTexture(texture_);
    }
    if (renderer_ != nullptr) {
      SDL_DestroyRenderer(renderer_);
    }
    if (window_ != nullptr) {
      SDL_DestroyWindow(window_);
    }
    SDL_Quit();
  }

  SdlWindow(const SdlWindow&) = delete;
  SdlWindow& operator=(const SdlWindow&) = delete;

  bool Open() {
    SDL_SetAppMetadata("Biblionaut", nullptr, nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "cannot open a window: %s\n", SDL_GetError());
      return false;
    }
    const core::DisplaySize size = layout_.Size();
    if (!SDL_CreateWindowAndRenderer(TitleFor(RefreshStage::kAfter, core::RefreshMode::kFull), size.width, size.height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window_, &renderer_)) {
      std::fprintf(stderr, "cannot open a window: %s\n", SDL_GetError());
      return false;
    }
    return MakeTexture();
  }

  core::Input& Buttons() noexcept override { return input_; }

  bool Pump() override {
    // The two clocks are read together, after SDL has gathered what has
    // happened, so that every event gathered is dated before this moment.
    SDL_PumpEvents();
    const std::uint64_t nowUs = clock_.NowUs();
    const std::uint64_t ticksNs = SDL_GetTicksNS();
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
          return false;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
          if (!OnKey(event.key, OnClock(event.key.timestamp, ticksNs, nowUs))) {
            return false;
          }
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
          // Into the units the device is laid out in.
          SDL_ConvertEventToRenderCoordinates(renderer_, &event);
          OnClick(event.button, OnClock(event.button.timestamp, ticksNs, nowUs));
          break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
          // Keys let go elsewhere are not heard of.
          keys_ = 0;
          mouse_ = 0;
          input_.Hold(0, nowUs);
          break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          stale_ = true;
          break;
        default:
          break;
      }
    }

    if (view_.Update(display_, clock_.NowUs())) {
      const core::DisplaySize size = view_.Size();
      if (!SDL_UpdateTexture(texture_, nullptr, view_.Pixels().data(),
                             static_cast<int>(size.width * sizeof(std::uint32_t)))) {
        std::fprintf(stderr, "warning: cannot update the window: %s\n", SDL_GetError());
      }
      SDL_SetWindowTitle(window_, TitleFor(view_.Stage(), display_.LastRefresh().mode));
      stale_ = true;
    }
    if (input_.Told() != drawnDown_) {
      stale_ = true;
    }
    if (stale_) {
      stale_ = false;
      Draw();
    }
    return true;
  }

 private:
  // A texture the size of the picture, and the window's units those of the
  // device around it, which keeps its shape however the window is sized.
  bool MakeTexture() {
    if (texture_ != nullptr) {
      SDL_DestroyTexture(texture_);
    }
    const core::DisplaySize picture = view_.Size();
    const core::DisplaySize size = layout_.Size();
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, picture.width,
                                 picture.height);
    if (texture_ == nullptr || !SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetRenderLogicalPresentation(renderer_, size.width, size.height, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
      std::fprintf(stderr, "cannot show the panel in the window: %s\n", SDL_GetError());
      return false;
    }
    stale_ = true;
    return true;
  }

  void SetColour(const Colour& colour) { SDL_SetRenderDrawColor(renderer_, colour.r, colour.g, colour.b, 0xFF); }

  void Draw() {
    SetColour(kOutside);
    SDL_RenderClear(renderer_);
    SetColour(kBody);
    const core::DisplaySize size = layout_.Size();
    const SDL_FRect body = ToFRect(Rect{.x = 0, .y = 0, .width = size.width, .height = size.height});
    SDL_RenderFillRect(renderer_, &body);
    const SDL_FRect panel = ToFRect(layout_.Panel());
    SDL_RenderTexture(renderer_, texture_, nullptr, &panel);

    drawnDown_ = input_.Told();
    for (const ButtonPlace& place : layout_.Buttons()) {
      const bool down = (drawnDown_ & core::ButtonBit(place.button)) != 0;
      SetColour(down ? kButtonDown : kButton);
      const SDL_FRect button = ToFRect(place.rect);
      SDL_RenderFillRect(renderer_, &button);
      // The label in the middle, in SDL's own letters, 8 by 8.
      const auto across = static_cast<float>(std::strlen(place.label) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);
      constexpr auto kHigh = static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);
      SetColour(down ? kLabelDown : kLabel);
      SDL_RenderDebugText(renderer_, button.x + ((button.w - across) / 2), button.y + ((button.h - kHigh) / 2),
                          place.label);
    }
    SDL_RenderPresent(renderer_);
  }

  // False if the window can no longer show the panel.
  bool OnKey(const SDL_KeyboardEvent& key, std::uint64_t timeUs) {
    if (key.repeat) {
      return true;
    }
    if (key.key == SDLK_R) {
      return key.down ? Turn() : true;
    }
    if (const std::optional<core::Button> button = ButtonFor(key.key)) {
      const unsigned bit = core::ButtonBit(*button);
      keys_ = static_cast<core::ButtonSet>(key.down ? (keys_ | bit) : (keys_ & ~bit));
      input_.Hold(static_cast<core::ButtonSet>(keys_ | mouse_), timeUs);
    }
    return true;
  }

  // A button drawn in the window is held while the mouse is, wherever it
  // goes meanwhile.
  void OnClick(const SDL_MouseButtonEvent& click, std::uint64_t timeUs) {
    if (click.button != SDL_BUTTON_LEFT) {
      return;
    }
    mouse_ = 0;
    if (click.down) {
      if (const std::optional<core::Button> button = layout_.ButtonAt(click.x, click.y)) {
        mouse_ = core::ButtonBit(*button);
      }
    }
    input_.Hold(static_cast<core::ButtonSet>(keys_ | mouse_), timeUs);
  }

  // Turns the device the other way, and the window with it.
  bool Turn() {
    const ViewTurn turn = view_.Turn() == ViewTurn::kAsRead ? ViewTurn::kAsPanel : ViewTurn::kAsRead;
    view_.SetTurn(turn);
    layout_ = DeviceLayout(display_.Size(), turn);
    int width = 0;
    int height = 0;
    if (SDL_GetWindowSize(window_, &width, &height)) {
      SDL_SetWindowSize(window_, height, width);
    }
    return MakeTexture();
  }

  core::Clock& clock_;
  SimDisplay& display_;
  WindowInput input_;
  PanelView view_;
  DeviceLayout layout_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  core::ButtonSet keys_ = 0;       // Held by keys.
  core::ButtonSet mouse_ = 0;      // Held by the mouse.
  core::ButtonSet drawnDown_ = 0;  // Drawn as held.
  bool stale_ = true;              // The window needs drawing again.
};

}  // namespace

const char* PanelWindow::KeysHelp() noexcept {
  return "Click the buttons, or use the keys: the arrows for Left, Right, Up and Down; Page Up and\n"
         "Page Down for Up and Down too; Return for Confirm; Escape or Backspace for Back; P for Power.\n"
         "R turns the device.\n";
}

std::unique_ptr<PanelWindow> PanelWindow::Open(core::Clock& clock, SimDisplay& display, core::Input& script) {
  auto window = std::make_unique<SdlWindow>(clock, display, script);
  if (!window->Open()) {
    return nullptr;
  }
  return window;
}

}  // namespace bn::hw
