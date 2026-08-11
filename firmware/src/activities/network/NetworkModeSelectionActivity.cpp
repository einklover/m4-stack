#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"
#include "util/TouchHitGeometry.h"

namespace {
constexpr int MENU_ITEM_COUNT = 3;
}  // namespace

void NetworkModeSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<NetworkModeSelectionActivity*>(param);
  self->displayTaskLoop();
}

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&NetworkModeSelectionActivity::taskTrampoline, "NetworkModeTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void NetworkModeSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void NetworkModeSelectionActivity::loop() {
  // Touch: item hit boxes match render() (itemHeight=62, centered startY)
  if (mappedInput.hasTouch()) {
    constexpr int itemHeight = 62;
    const int pageHeight = renderer.getScreenHeight();
    const int startY = (pageHeight - (MENU_ITEM_COUNT * itemHeight)) / 2 + 20;

    M4ListTouchPolicy::Event te{};
    te.backGesture = mappedInput.wasBackGesture();
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTouchDown(tx, ty)) {
      te.touchDown = true;
      te.x = tx;
      te.y = ty;
    } else if (mappedInput.wasScreenTapped(tx, ty)) {
      te.tap = true;
      te.x = tx;
      te.y = ty;
    }

    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = startY;
    layout.listHeight = MENU_ITEM_COUNT * itemHeight;
    layout.rowStep = itemHeight;
    layout.itemCount = MENU_ITEM_COUNT;
    layout.selectedIndex = selectedIndex;
    layout.maxVisible = MENU_ITEM_COUNT;

    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::Back) {
      onCancel();
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectedIndex != hit) {
        selectedIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex = hit;
      NetworkMode mode = NetworkMode::CREATE_HOTSPOT;
      if (selectedIndex == 1) mode = NetworkMode::JOIN_NETWORK;
      else if (selectedIndex == 2) mode = NetworkMode::CONNECT_CALIBRE;
      onModeSelected(mode);
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    NetworkMode mode = NetworkMode::CREATE_HOTSPOT;
    if (selectedIndex == 1) {
      mode = NetworkMode::JOIN_NETWORK;
    } else if (selectedIndex == 2) {
      mode = NetworkMode::CONNECT_CALIBRE;
    }
    onModeSelected(mode);
    return;
  }

  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  if (prevPressed) {
    selectedIndex = (selectedIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
    updateRequired = true;
  } else if (nextPressed) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEM_COUNT;
    updateRequired = true;
  }
}

void NetworkModeSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void NetworkModeSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kWifiFunctionSettings), true, EpdFontFamily::BOLD);

  // Draw subtitle
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 50, L(Str::kHowToConnect));

  // Draw menu items centered on screen
  constexpr int itemHeight = 62;  // Height for each menu item
  const int startY = (pageHeight - (MENU_ITEM_COUNT * itemHeight)) / 2 + 20;

    const char* MENU_ITEMS[MENU_ITEM_COUNT] = {
        L(Str::kMode1), L(Str::kMode2),
        L(Str::kMode3),
    };

  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    const int itemY = startY + i * itemHeight;
    const bool isSelected = (i == selectedIndex);

    // Draw selection highlight (black fill) for selected item
    if (isSelected) {
      renderer.fillRect(20, itemY - 2, pageWidth - 40, itemHeight - 6);
    }

    // Draw text: black=false (white text) when selected (on black background)
    //            black=true (black text) when not selected (on white background)
    M4UiText::draw(renderer, UI_10_FONT_ID, 30, itemY + 10, MENU_ITEMS[i], /*black=*/!isSelected);
  }

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kSelect), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
