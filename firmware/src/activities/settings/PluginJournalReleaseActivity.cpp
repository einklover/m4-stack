#include "PluginJournalReleaseActivity.h"

#ifdef CROSSPOINT_MURPHY_M4

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "MappedInputManager.h"
#include "apps/M4xInstallJournal.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xRegistry.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

namespace {

constexpr int kMaxRows = 8;

const char* kTitle = "修复未完成插件安装记录";
const char* kWarn1 = "只归档并解除阻塞";
const char* kWarn2 = "不保证找回丢失文件";
const char* kWarn3 = "不删 apps_data，不自动安装或卸载";

bool registryHas(const std::string& id, void* ud) {
  const auto* apps = static_cast<const std::vector<M4xInstalledApp>*>(ud);
  return apps && M4xRegistry::find(*apps, id) != nullptr;
}

}  // namespace

void PluginJournalReleaseActivity::taskTrampoline(void* param) {
  static_cast<PluginJournalReleaseActivity*>(param)->displayTaskLoop();
}

void PluginJournalReleaseActivity::reloadRows() {
  rows_.clear();
  listOk_ = false;
  truncated_ = false;
  status_.clear();
  auto apps = M4xRegistry::load();
  std::vector<M4xInstallJournal::PendingJournalId> ids;
  if (!M4xInstallJournal::listPendingJournalIds(ids, &registryHas, &apps)) {
    status_ = "无法读取安装记录";
    m4JournalReleaseSetCount(ui_, 0);
    return;
  }
  listOk_ = true;
  if (ids.size() > static_cast<size_t>(kMaxRows)) truncated_ = true;
  const size_t n = ids.size() < static_cast<size_t>(kMaxRows) ? ids.size() : static_cast<size_t>(kMaxRows);
  rows_.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    rows_.push_back(Row{ids[i].id, ids[i].inRegistry});
  }
  m4JournalReleaseSetCount(ui_, static_cast<int>(rows_.size()));
  if (rows_.empty()) status_ = "没有未完成记录";
}

void PluginJournalReleaseActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  ui_ = M4JournalReleaseUi{};
  reloadRows();
  updateRequired_ = true;
  xTaskCreate(&PluginJournalReleaseActivity::taskTrampoline, "JnlRel", 4096, this, 1, &displayTaskHandle_);
}

void PluginJournalReleaseActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  vSemaphoreDelete(renderingMutex_);
  renderingMutex_ = nullptr;
}

void PluginJournalReleaseActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_) {
      updateRequired_ = false;
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void PluginJournalReleaseActivity::applyRelease() {
  if (!ui_.releaseRequested) return;
  const int index = ui_.releaseIndex;
  ui_.releaseRequested = false;
  ui_.releaseIndex = -1;
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    status_ = "未选择记录";
    m4JournalReleaseShowResult(ui_);
    updateRequired_ = true;
    return;
  }
  const std::string id = rows_[static_cast<size_t>(index)].id;
  std::string err;
  const bool ok = M4xInstaller::archiveAndReleasePending(id, err);
  m4JournalReleaseShowResult(ui_);
  reloadRows();
  ui_.page = M4JournalReleasePage::Result;
  if (ok) {
    status_ = "已解除阻塞";
    Serial.printf("[M4x] journal release ok id=%s\n", id.c_str());
  } else {
    status_ = std::string("归档失败，记录仍保留 ") + err;
    Serial.printf("[M4x] journal release fail id=%s err=%s\n", id.c_str(), err.c_str());
  }
  updateRequired_ = true;
}

void PluginJournalReleaseActivity::render() {
  const int pageW = renderer.getScreenWidth();
  const auto metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageW, metrics.headerHeight}, kTitle);

  int y = metrics.topPadding + metrics.headerHeight + 12;
  M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, kWarn1, true);
  y += 24;
  M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, kWarn2, true);
  y += 24;
  M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, kWarn3, true);
  y += 32;

  if (ui_.page == M4JournalReleasePage::Confirm) {
    const auto& row = rows_[static_cast<size_t>(ui_.selected)];
    M4UiText::draw(renderer, NOTOSANS_18_FONT_ID, 16, y, "确认解除这条记录？", true, EpdFontFamily::BOLD);
    y += 36;
    M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, row.id.c_str(), true);
    y += 28;
    M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, row.inRegistry ? "已在插件列表" : "未在插件列表", true);
    y += 40;
    M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, "确认后才归档。返回则取消。", true);
  } else if (ui_.page == M4JournalReleasePage::Result) {
    M4UiText::draw(renderer, NOTOSANS_18_FONT_ID, 16, y, status_.c_str(), true, EpdFontFamily::BOLD);
  } else if (!listOk_ || rows_.empty()) {
    M4UiText::draw(renderer, NOTOSANS_18_FONT_ID, 16, y, status_.c_str(), true, EpdFontFamily::BOLD);
  } else {
    if (truncated_) {
      M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, "仅显示前 8 条", true);
      y += 24;
    }
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const bool sel = i == ui_.selected;
      if (sel) renderer.fillRect(8, y - 4, pageW - 16, 28);
      const char* mark = rows_[static_cast<size_t>(i)].inRegistry ? "" : " [未登记]";
      char line[120];
      snprintf(line, sizeof(line), "%s%s", rows_[static_cast<size_t>(i)].id.c_str(), mark);
      M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 16, y, line, !sel);
      y += 32;
    }
  }

  const auto labels = mappedInput.mapLabels("返回", ui_.page == M4JournalReleasePage::Confirm ? "确认解除" : "选择",
                                            "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PluginJournalReleaseActivity::loop() {
  if (ui_.releaseRequested) applyRelease();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    m4JournalReleaseCancel(ui_);
    if (ui_.leave) {
      goBack_();
      return;
    }
    updateRequired_ = true;
    return;
  }

  if (ui_.page == M4JournalReleasePage::List) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      m4JournalReleaseMove(ui_, -1);
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      m4JournalReleaseMove(ui_, 1);
      updateRequired_ = true;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    m4JournalReleaseActivate(ui_);
    updateRequired_ = true;
    return;
  }
}

#else

void PluginJournalReleaseActivity::taskTrampoline(void*) {}
void PluginJournalReleaseActivity::reloadRows() {}
void PluginJournalReleaseActivity::onEnter() {}
void PluginJournalReleaseActivity::onExit() {}
void PluginJournalReleaseActivity::displayTaskLoop() {}
void PluginJournalReleaseActivity::applyRelease() {}
void PluginJournalReleaseActivity::render() {}
void PluginJournalReleaseActivity::loop() {}

#endif
