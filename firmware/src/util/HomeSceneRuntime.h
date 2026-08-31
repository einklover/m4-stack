#pragma once

// Compatibility surface for old Home callers. The canonical M4TH/SCENE parser
// and package contracts live in ui/scene/UiScenePackage.h.
#include "ui/scene/UiScenePackage.h"

namespace HomeSceneRuntime {

using UiScene::ActionId;
using UiScene::ActionTarget;
using UiScene::BindingId;
using UiScene::DataState;
using UiScene::ItemContext;
using UiScene::NumericBinding;
using UiScene::Rect;
using UiScene::RepeatInfo;
using UiScene::SceneCommand;
using UiScene::SceneHeader;
using UiScene::SectionInfo;
using UiScene::TextView;
using UiScene::UiScenePackage;

static constexpr BindingId kInvalidBindingId = UiScene::kInvalidBindingId;
static constexpr ActionId kInvalidActionId = UiScene::kInvalidActionId;
static constexpr uint32_t kMagic = UiScene::kM4thMagic;
static constexpr uint16_t kVersion = UiScene::kM4thVersion;
static constexpr uint16_t kHeaderSize = UiScene::kM4thHeaderSize;
static constexpr uint16_t kScreenW = UiScene::kScreenWidth;
static constexpr uint16_t kScreenH = UiScene::kScreenHeight;
static constexpr uint32_t kMaxTotal = UiScene::kMaxPackageBytes;
static constexpr uint16_t kSceneVersion = UiScene::kSceneVersion;
static constexpr uint32_t kSectionMeta = UiScene::kMetaSection;
static constexpr uint32_t kSectionStrings = UiScene::kStringsSection;
static constexpr uint32_t kSectionSlots = UiScene::kSlotsSection;
static constexpr uint32_t kSectionAssets = UiScene::kAssetsSection;
static constexpr uint32_t kSectionAssetData = UiScene::kAssetDataSection;
static constexpr uint32_t kSectionScene = UiScene::kSceneSection;
static constexpr uint32_t kSectionInteractions = UiScene::kInteractionsSection;
static constexpr uint8_t kNodeClear = UiScene::kNodeClear;
static constexpr uint8_t kNodeBitmap = UiScene::kNodeBitmap;
static constexpr uint8_t kNodeLine = UiScene::kNodeLine;
static constexpr uint8_t kNodeRect = UiScene::kNodeRect;
static constexpr uint8_t kNodeRoundRect = UiScene::kNodeRoundRect;
static constexpr uint8_t kNodeText = UiScene::kNodeText;
static constexpr uint8_t kNodeCover = UiScene::kNodeCover;
static constexpr uint8_t kNodeProgress = UiScene::kNodeProgress;
static constexpr uint8_t kNodeIcon = UiScene::kNodeIcon;
static constexpr uint8_t kNodeBattery = UiScene::kNodeBattery;
static constexpr uint8_t kNodeGroup = UiScene::kNodeGroup;
static constexpr uint8_t kNodeRepeat = UiScene::kNodeRepeat;
static constexpr uint8_t kFlagVisibleIf = UiScene::kFlagVisibleIf;
static constexpr uint8_t kFlagAction = UiScene::kFlagAction;

using UiScene::findSection;
using UiScene::forEachCommand;
using UiScene::forEachCommandVoid;
using UiScene::forEachRepeatChild;
using UiScene::hasSceneSection;
using UiScene::isValidM4TH;
using UiScene::parseRepeatInfo;
using UiScene::parseSceneHeader;
using UiScene::readU16;
using UiScene::readU32;
using UiScene::sceneCommandActionArgBinding;
using UiScene::sceneCommandActionHasArg;
using UiScene::sceneCommandActionId;
using UiScene::sceneCommandHasAction;
using UiScene::sceneCommandHasVisibleIf;
using UiScene::sceneCommandVisibleBinding;
using UiScene::validatePackage;

template <typename Fn>
inline bool executeScene(const uint8_t* data, size_t len, Fn&& fn) {
  return UiScene::forEachCommand(data, len, static_cast<Fn&&>(fn));
}

} // namespace HomeSceneRuntime
