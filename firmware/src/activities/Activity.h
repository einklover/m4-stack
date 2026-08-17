#pragma once

#include <HardwareSerial.h>

#include <string>
#include <utility>

#include "util/M4FooterTouchPolicy.h"
#include "util/M4ReaderFrontlightGesture.h"
#include "util/M4TouchNavigation.h"
#include "util/M4UiRuntimePolicy.h"

class MappedInputManager;
class GfxRenderer;
class ActivityWithSubactivity;

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 private:
  ActivityWithSubactivity* parentActivity_ = nullptr;
  friend class ActivityWithSubactivity;
  void setParentActivity(ActivityWithSubactivity* parent) { parentActivity_ = parent; }

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter() {
    M4TouchNavigation::activateForActivity(showTouchNavigation());
    M4UiRuntimePolicy::setTextScalePercent(uiTextScalePercent());
    M4ReaderFrontlightGesture::setEnabled(isReaderBodyActivity());
    M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
    Serial.printf("[%lu] [ACT] Entering activity: %s fullscreen=%d nav=%d ui_scale=%d%% light_gesture=%d footer_mask=0x%02x\n",
                  millis(), name.c_str(), isFullscreenActivity() ? 1 : 0,
                  showTouchNavigation() ? 1 : 0, uiTextScalePercent(), isReaderBodyActivity() ? 1 : 0,
                  static_cast<unsigned>(touchFooterButtonsMask()));
  }
  virtual void onExit() {
    M4TouchNavigation::setMode(M4TouchNavigation::Mode::None);
    M4UiRuntimePolicy::setTextScalePercent(100);
    M4ReaderFrontlightGesture::setEnabled(false);
    M4FooterTouchPolicy::setMask(0);
    Serial.printf("[%lu] [ACT] Exiting activity: %s\n", millis(), name.c_str());
  }
  virtual void loop() {}
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  // True only for the actual paged/text body surface. ReaderActivity itself is
  // a router/owner and overrides this false; nested menus/settings remain false.
  virtual bool isReaderBodyActivity() const { return isReaderActivity(); }
  virtual bool isHomeActivity() const { return false; }
  // Explicit viewport ownership declaration. A full-screen Activity owns the
  // complete app surface and must not receive the global Murphy Back/Home bar.
  virtual bool isFullscreenActivity() const { return false; }
  // Logical button mask for an activity-owned bottom hint bar that should also
  // behave as a touch control. Zero keeps legacy/hardware-only button hints.
  virtual uint8_t touchFooterButtonsMask() const { return 0; }
  // Reader body and Home intentionally stay visually clean. Every other normal
  // activity gets explicit touch navigation unless it explicitly owns a
  // full-screen viewport or is a special boot/error surface overriding policy.
  virtual bool showTouchNavigation() const {
    return !isFullscreenActivity() && !isReaderActivity() && !isHomeActivity();
  }
  // Activity-local chrome scale. This affects only M4UiText draw-time metrics;
  // it never changes SETTINGS reader size or allocates another runtime TTF face.
  virtual int uiTextScalePercent() const { return 100; }
  // Structured UI dump for m4adb `ui` (JSON object body, no outer braces required
  // to be complete alone — implementors return a full JSON object string).
  // Used for automation without OCR/screenshot text recognition.
  virtual std::string debugUiJson() { return "{}"; }
  const std::string& getName() const { return name; }
  ActivityWithSubactivity* getParentActivity() const { return parentActivity_; }
};