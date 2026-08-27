#include "JianGuoYunSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"
#include "util/M4UiText.h"

#include "components/UITheme.h"

// 定义坚果云配置菜单选项（只保留账号和密码，不再提示输入目录）
namespace {
constexpr int MENU_ITEMS = 2;

// 扩展CrossPointSettings，新增坚果云配置字段（最小侵入，复用原有存储）
// 若原有SETTINGS结构体未定义这些字段，需先在CrossPointSettings.h中添加：
// char jgUsername[64] = "";    // 坚果云账号（邮箱）
// char jgAppPassword[64] = ""; // 坚果云应用密码
// char jgBookFolder[128] = "/KOReader/Books/"; // 电子书文件夹路径
}  // namespace

void JianGuoYunSettingsActivity::taskTrampoline(void* param) {
    auto* self = static_cast<JianGuoYunSettingsActivity*>(param);
    self->displayTaskLoop();
}

void JianGuoYunSettingsActivity::onEnter() {
    ActivityWithSubactivity::onEnter();

    renderingMutex = xSemaphoreCreateMutex();
    selectedIndex = 0;
    updateRequired = true;

    // 创建显示任务（和Calibre一致的栈大小、优先级）
    xTaskCreate(&JianGuoYunSettingsActivity::taskTrampoline, "JianGuoYunSettingsTask",
                4096,               // Stack size 复用原有配置
                this,               // Parameters
                1,                  // Priority
                &displayTaskHandle  // Task handle
    );
}

void JianGuoYunSettingsActivity::onExit() {
    ActivityWithSubactivity::onExit();

    // 清理资源（和Calibre逻辑完全一致）
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (displayTaskHandle) {
        vTaskDelete(displayTaskHandle);
        displayTaskHandle = nullptr;
    }
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
}

void JianGuoYunSettingsActivity::loop() {
    if (subActivity) {
        pumpSubActivityFrame();
        return;
    }

    // 返回按钮逻辑
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        onBack();
        return;
    }

    // 确认按钮逻辑
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        handleSelection();
        return;
    }

    // 上下/左右键切换选项（和Calibre交互逻辑一致）
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
        updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
        updateRequired = true;
    }
}

void JianGuoYunSettingsActivity::handleSelection() {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);

    if (selectedIndex == 0) {
        // 配置坚果云账号（邮箱）
        exitActivity();
        enterNewActivity(new KeyboardEntryActivity(
            renderer, mappedInput, L(Str::kEnterJianGuoAccount), SETTINGS.jgUsername, 10,
            63,     // 最大长度63，和Calibre一致
            false,  // 非密码模式
            [this](const std::string& username) {
                // 保存账号到配置
                strncpy(SETTINGS.jgUsername, username.c_str(), sizeof(SETTINGS.jgUsername) - 1);
                SETTINGS.jgUsername[sizeof(SETTINGS.jgUsername) - 1] = '\0';
                SETTINGS.saveToFile(); // 复用原有保存逻辑
                exitActivity();
                updateRequired = true;
            },
            [this]() {
                // 取消输入
                exitActivity();
                updateRequired = true;
            }));
    } else if (selectedIndex == 1) {
        // 配置坚果云应用密码
        exitActivity();
        enterNewActivity(new KeyboardEntryActivity(
            renderer, mappedInput, L(Str::kEnterAppPassword), SETTINGS.jgAppPassword, 10,
            63,     // 最大长度63
            false,   // 密码模式（输入时隐藏）
            [this](const std::string& password) {
                // 保存应用密码到配置
                strncpy(SETTINGS.jgAppPassword, password.c_str(), sizeof(SETTINGS.jgAppPassword) - 1);
                SETTINGS.jgAppPassword[sizeof(SETTINGS.jgAppPassword) - 1] = '\0';
                SETTINGS.saveToFile();
                exitActivity();
                updateRequired = true;
            },
            [this]() {
                // 取消输入
                exitActivity();
                updateRequired = true;
            }));
    }

    xSemaphoreGive(renderingMutex);
}

void JianGuoYunSettingsActivity::displayTaskLoop() {
    // 和Calibre一致的显示循环逻辑
    while (true) {
        if (updateRequired && !subActivity) {
            updateRequired = false;
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            render();
            xSemaphoreGive(renderingMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void JianGuoYunSettingsActivity::render() {
    renderer.clearScreen();
    const auto pageWidth = renderer.getScreenWidth();

    // 标题（和Calibre风格一致）
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kJianGuoConfig), true, EpdFontFamily::BOLD);

    // 选中项高亮（和Calibre交互视觉一致）
    renderer.fillRect(0, 70 + selectedIndex * 40 - 2, pageWidth - 1, 40);

    // 绘制所有配置项
    const char* menuNames[MENU_ITEMS] = {L(Str::kJianGuoAccount), L(Str::kAppPassword)};
    for (int i = 0; i < MENU_ITEMS; i++) {
        const int settingY = 70 + i * 40;
        const bool isSelected = (i == selectedIndex);

        // 绘制选项名称
        M4UiText::draw(renderer, UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

        // 绘制配置状态（已配置/未配置）
        const char* status = L(Str::kNotConfigured);
        if (i == 0) {
            status = (strlen(SETTINGS.jgUsername) > 0) ? L(Str::kConfigured) : L(Str::kNotConfigured);
        } else if (i == 1) {
            status = (strlen(SETTINGS.jgAppPassword) > 0) ? L(Str::kConfigured) : L(Str::kNotConfigured);
        }
        const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, status);
        M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    }

    // 按钮提示（中文适配，和Calibre一致）
    const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kSelect), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
}
