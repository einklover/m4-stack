#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

// Global activity stack pop (main.cpp)
void exitActivity();

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (renderer.getScreenHeight() - height) / 2;

  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, top, text.c_str(), true, style);
  renderer.displayBuffer(refreshMode);
}

void FullScreenMessageActivity::loop() {
  // Dismiss on any confirm/back key or full-screen tap / edge-back.
  // No discrete buttons are drawn (message-only); entire surface is the hit target.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    exitActivity();
    return;
  }
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      exitActivity();
      return;
    }
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) &&
        TouchHitGeometry::fullScreenDismissFromPoint(tx, ty, renderer.getScreenWidth(),
                                                     renderer.getScreenHeight())) {
      exitActivity();
    }
  }
}
