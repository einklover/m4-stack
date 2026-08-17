#pragma once

#include <HalGPIO.h>
#include <cstdint>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  enum class SwipeDir { None, Left, Right, Up, Down };
  enum class RowTouch : uint8_t { None, Down, Tap };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& rendererRef) : gpio(gpio), renderer(&rendererRef) {}

  // Backward-compatible single-arg form used by older call sites that only need buttons.
  explicit MappedInputManager(HalGPIO& gpio);

  void update() const { gpio.update(); }
  // Call once per main-loop frame after gpio.update() so touch/swipe events are
  // decoded once and can be read by both system navigation and the activity.
  void beginFrame();
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // Touch boundary (false/no-op on non-touch hardware)
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  unsigned long lastScreenTouchHeldMs() const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  SwipeDir wasSwipe() const;
  bool wasMenuGesture() const;
  // Right-edge swipe left → system back. On M4 non-reader chrome, the explicit
  // Back control is reported through this same path so the main loop can keep
  // using its existing synthetic-Back routing.
  bool wasBackGesture() const;
  // True only for the edge swipe, not for the on-screen Back control.
  bool wasBackSwipeGesture() const;
  // Returns the logical transition direction for the edge swipe:
  // 1 = left→right, 0 = right→left, -1 = not a back swipe.
  int backSwipeAnimationDirection() const;
  // Bottom-edge swipe up → system home. On touch screens with the shared
  // bottom navigation bar, tapping Home is reported through this same path.
  bool wasHomeGesture() const;
  // True only for the bottom-edge swipe, not for the on-screen Home control.
  bool wasHomeSwipeGesture() const;
  // Make wasPressed/wasReleased(Back) true this frame so every activity's button
  // path handles edge-back without per-page wiring.
  void pulseSyntheticBack();
  bool wasTouchActivity() const;

#if defined(CROSSPOINT_MURPHY_M4)
  // One-frame synthetic input for USB serial debug bridge only (runtime-authorized).
  // Physical input continues to work; synthetic events clear in beginFrame().
  // Returns false with busy=true when a prior event is still pending or rate-limited.
  bool injectSyntheticTap(int x, int y, bool& busy);
  bool injectSyntheticSwipe(int sx, int sy, int ex, int ey, bool& busy);
  bool injectSyntheticKey(Button button, bool& busy);
  bool hasPendingSyntheticInput() const;
#endif

 private:
  HalGPIO& gpio;
  const GfxRenderer* renderer = nullptr;

  // Per-frame tap cache. Global navigation checks run before activity loop();
  // caching prevents those checks from consuming an ordinary list/button tap.
  mutable bool tapCacheValid = false;
  mutable bool tapCacheHas = false;
  mutable int tapX = 0, tapY = 0;

  // Per-frame swipe cache (wasSwipe is multi-readable within one frame)
  mutable bool swipeCacheValid = false;
  mutable bool swipeCacheHas = false;
  mutable int swipeSx = 0, swipeSy = 0, swipeEx = 0, swipeEy = 0;
  // Synthetic Back for global edge-back → button path
  mutable bool syntheticBack = false;
  // Preserve touch duration after the release event is consumed.
  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;

#if defined(CROSSPOINT_MURPHY_M4)
  enum class SynthKind : uint8_t { None, Tap, Swipe, Key };
  mutable SynthKind synthKind_ = SynthKind::None;
  mutable int synthTapX_ = 0;
  mutable int synthTapY_ = 0;
  mutable int synthSwipeSx_ = 0;
  mutable int synthSwipeSy_ = 0;
  mutable int synthSwipeEx_ = 0;
  mutable int synthSwipeEy_ = 0;
  mutable Button synthKey_ = Button::Back;
  mutable unsigned long synthLastInjectMs_ = 0;
  mutable bool synthEverInjected_ = false;
  static constexpr unsigned long kSynthMinIntervalMs = 40;
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  void rememberTouchHeldTime() const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
};
