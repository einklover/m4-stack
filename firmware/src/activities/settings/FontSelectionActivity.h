#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class FontSelectionActivity : public Activity {
 public:
  enum class Target { Reader, SystemUi };

  FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& inputManager, std::function<void(bool)> onClose,
                        Target target = Target::Reader);
  ~FontSelectionActivity() override;
  void onEnter() override;
  void loop() override;
  void render() const;

 private:
  std::function<void(bool)> onClose;
  Target target_ = Target::Reader;
  std::vector<std::string> fontFamilies;
  int selectedIndex = 0;
  int scrollOffset = 0;
  int itemsPerPage = 8;  // will be recalculated in render based on screen height
  void saveAndExit();
  void drawScrollBar(int totalItems, int startY, int areaHeight) const;
};
