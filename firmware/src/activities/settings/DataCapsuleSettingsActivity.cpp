#include "DataCapsuleSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"
#include "util/M4UiText.h"

#include "components/UITheme.h"

// 定义数据胶囊配置菜单选项
namespace {
constexpr int MENU_ITEMS = 3;
// 默认WebDAV地址
constexpr const char* DEFAULT_DC_URL = "https://data.cstcloud.cn/dav";
}  // namespace

void DataCapsuleSettingsActivity::taskTrampoline(void* param) {
    auto* self = static_cast<DataCapsuleSettingsActivity*>(param);
    self->displayTaskLoop();
}

void DataCapsuleSettingsActivity::onEnter() {
    ActivityWithSubactivity::onEnter();

    renderingMutex = xSemaphoreCreateMutex();
    selectedIndex = 0;
    updateRequired = true;

    // 创建显示任务
    xTaskCreate(&DataCapsuleSettingsActivity::taskTrampoline, "DataCapsuleSettingsTask",
                4096,               // Stack size
                this,               // Parameters
                1,                  // Priority
                &displayTaskHandle  // Task handle
    );
}

void DataCapsuleSettingsActivity::onExit() {
    if (displayTaskHandle) {
        vTaskDelete(displayTaskHandle);
        displayTaskHandle = nullptr;
    }
    if (renderingMutex) {
        vSemaphoreDelete(renderingMutex);
        renderingMutex = nullptr;
    }
    ActivityWithSubactivity::onExit();
}

void DataCapsuleSettingsActivity::displayTaskLoop() {
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

void DataCapsuleSettingsActivity::loop() {
    if (subActivity) {
        pumpSubActivityFrame();
        return;
    }

    // 返回按键逻辑
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        onBack();
        return;
    }

    // 确认按键逻辑
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        handleSelection();
        return;
    }

    // 上下键切换选项
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
        updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
        updateRequired = true;
    }
}

void DataCapsuleSettingsActivity::handleSelection() {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);

    if (selectedIndex == 0) {
        // 配置用户名
        exitActivity();
        enterNewActivity(new KeyboardEntryActivity(
            renderer, mappedInput, L(Str::kEnterUsername), SETTINGS.dcUsername, 10,
            63,     // 最大长度63
            false,  // 非密码模式
            [this](const std::string& username) {
                // 保存用户名到配置
                strncpy(SETTINGS.dcUsername, username.c_str(), sizeof(SETTINGS.dcUsername) - 1);
                SETTINGS.dcUsername[sizeof(SETTINGS.dcUsername) - 1] = '\0';
                SETTINGS.saveToFile();
                exitActivity();
                updateRequired = true;
            },
            [this]() {
                // 取消输入
                exitActivity();
                updateRequired = true;
            }));
    } else if (selectedIndex == 1) {
        // 配置密码
        exitActivity();
        enterNewActivity(new KeyboardEntryActivity(
            renderer, mappedInput, L(Str::kEnterPassword), SETTINGS.dcPassword, 10,
            63,     // 最大长度63
            false,  // 非密码模式（但显示时隐藏）
            [this](const std::string& password) {
                // 保存密码到配置
                strncpy(SETTINGS.dcPassword, password.c_str(), sizeof(SETTINGS.dcPassword) - 1);
                SETTINGS.dcPassword[sizeof(SETTINGS.dcPassword) - 1] = '\0';
                SETTINGS.saveToFile();
                exitActivity();
                updateRequired = true;
            },
            [this]() {
                // 取消输入
                exitActivity();
                updateRequired = true;
            }));
    } else if (selectedIndex == 2) {
        // 配置WebDAV地址
        exitActivity();
        enterNewActivity(new KeyboardEntryActivity(
            renderer, mappedInput, L(Str::kEnterWebdavUrl), SETTINGS.dcWebdavUrl, 10,
            127,    // 最大长度127
            false,  // 非密码模式
            [this](const std::string& url) {
                // 保存WebDAV地址到配置
                strncpy(SETTINGS.dcWebdavUrl, url.c_str(), sizeof(SETTINGS.dcWebdavUrl) - 1);
                SETTINGS.dcWebdavUrl[sizeof(SETTINGS.dcWebdavUrl) - 1] = '\0';
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

void DataCapsuleSettingsActivity::render() {
    renderer.clearScreen();
    const auto pageWidth = renderer.getScreenWidth();

    // 标题
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kDataCapsuleConfig), true, EpdFontFamily::BOLD);

    // 选中项高亮
    renderer.fillRect(0, 70 + selectedIndex * 40 - 2, pageWidth - 1, 40);

    // 绘制所有配置项
    const char* menuNames[MENU_ITEMS] = {L(Str::kDataCapsuleUsername2), L(Str::kPassword), L(Str::kWebdavUrl)};
    for (int i = 0; i < MENU_ITEMS; i++) {
        const int settingY = 70 + i * 40;
        const bool isSelected = (i == selectedIndex);

        // 绘制选项名称
        M4UiText::draw(renderer, UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

        // 绘制配置状态（已配置/未配置）
        const char* status = L(Str::kNotConfigured);
        if (i == 0) {
            status = (strlen(SETTINGS.dcUsername) > 0) ? L(Str::kConfigured) : L(Str::kNotConfigured);
        } else if (i == 1) {
            status = (strlen(SETTINGS.dcPassword) > 0) ? L(Str::kConfigured) : L(Str::kNotConfigured);
        } else if (i == 2) {
            status = (strlen(SETTINGS.dcWebdavUrl) > 0) ? L(Str::kConfigured) : L(Str::kNotConfigured);
        }
        const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, status);
        M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    }

    // 按钮提示
    const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kSelect), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
}
