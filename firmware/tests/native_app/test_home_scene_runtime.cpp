#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Host-safe PROGMEM stub if avr header missing is handled by HomeSceneRuntime.h
#include "util/HomeSceneRuntime.h"
#include "generated/murphy_default_m4theme.h"

using namespace HomeSceneRuntime;

int main() {
  const uint8_t* data = murphy_default_m4theme;
  size_t len = murphy_default_m4theme_len;

  // 1) package valid
  assert(validatePackage(data, len) && "murphy_default package should be valid");
  // alternative isValid check
  assert(isValidM4TH(data, len));

  // 2) SCENE section exists
  SectionInfo sceneSec{};
  bool found = findSection(data, len, kSectionScene, &sceneSec);
  assert(found && "SCENE section (type 6) should exist in murphy_default");
  assert(sceneSec.length >= 8);
  printf("SCENE section offset=%u len=%u count=%u\n", (unsigned)sceneSec.offset, (unsigned)sceneSec.length, (unsigned)sceneSec.count);

  // 3) scene version 1
  SceneHeader hdr{};
  assert(parseSceneHeader(data, len, &hdr) && "parseSceneHeader should succeed");
  assert(hdr.version == 1 && "scene version must be 1");
  assert(hdr.version == kSceneVersion);
  printf("scene header version=%u count=%u\n", (unsigned)hdr.version, (unsigned)hdr.commandCount);

  // 4) command count matches package (descriptor count vs header count)
  assert(hdr.commandCount == sceneSec.count && "header count must match descriptor count");
  // Also expect 18 for this fixture (from compile_home_theme)
  assert(hdr.commandCount == 18 && "murphy_default should have 18 scene commands");

  // 5) command iteration order: exactly matches decoded command count, begins with clear
  SceneCommand cmds[64];
  size_t cmdCount = 0;
  bool ok = forEachCommand(data, len, [&](const SceneCommand& cmd) -> bool {
    assert(cmdCount < 64);
    cmds[cmdCount++] = cmd;
    return true; // continue
  });
  assert(ok && "forEachCommand should succeed on valid package");
  assert(cmdCount == hdr.commandCount && "iteration count must exactly match header count");
  assert(cmdCount == sceneSec.count);

  // check order: first is CLEAR (0)
  assert(cmdCount > 0);
  assert(cmds[0].type == kNodeClear && "first command should be CLEAR");
  // clear before text/line/cover etc.
  bool hasText = false, hasLine = false, hasCover = false, hasProgress = false, hasRepeat = false, hasBattery = false, hasRoundRect = false;
  for (size_t i=0;i<cmdCount;++i) {
    auto &c = cmds[i];
    if (c.type == kNodeText) hasText = true;
    if (c.type == kNodeLine) hasLine = true;
    if (c.type == kNodeCover) hasCover = true;
    if (c.type == kNodeProgress) hasProgress = true;
    if (c.type == kNodeRepeat) hasRepeat = true;
    if (c.type == kNodeBattery) hasBattery = true;
    if (c.type == kNodeRoundRect) hasRoundRect = true;
  }

  bool foundProgressTextBinding = false;
  bool foundNumericProgressBinding = false;
  for (size_t i=0;i<cmdCount;++i) {
    const auto& command = cmds[i];
    if (command.type == kNodeText && command.flags == 0 && command.payloadLen >= 16 &&
        UiScene::readU16(command.payload + 2) == 302) {
      assert(pgm_read_byte(command.payload + 12) == 1);
      assert(pgm_read_byte(command.payload + 13) == 16);
      foundProgressTextBinding = true;
    }
    if (command.type == kNodeProgress && command.flags == 0 && command.payloadLen >= 11 &&
        UiScene::readU16(command.payload + 2) == 325) {
      assert(pgm_read_byte(command.payload + 10) == 15);
      foundNumericProgressBinding = true;
    }
  }
  assert(foundProgressTextBinding && foundNumericProgressBinding);
  assert(hasText && "should contain text");
  assert(hasLine && "should contain line");
  assert(hasCover && "should contain cover");
  assert(hasProgress && "should contain progress");
  assert(hasRepeat && "should contain repeat");
  assert(hasBattery && "should contain battery");
  assert(hasRoundRect && "should contain round_rect");

  // verify exact order matches known default scene (18 entries)
  // order from decode: 0,5,9,2,4,6,5,5,5,5,5,7,5,5,11,5,5,11 (clear before text/line/cover/progress/repeat)
  const uint8_t expected[18] = {0,5,9,2,4,6,5,5,5,5,5,7,5,5,11,5,5,11};
  for (size_t i=0;i<cmdCount;++i) {
    assert(cmds[i].type == expected[i] && "command order must exactly match serialized order");
  }

  // verify 4-byte padding handling: payloadLen + header padded
  // forEach should have validated padding and not misaligned
  // also verify flags/payload pointer are accessible
  for (size_t i=0;i<cmdCount;++i) {
    auto &c = cmds[i];
    // payload pointer must be within package bounds
    // we check via pgm_read_byte safe access (host is direct)
    if (c.payloadLen > 0) {
      assert(c.payload != nullptr);
      // try reading first byte via pgm_read_byte
      volatile uint8_t b = pgm_read_byte(c.payload);
      (void)b;
    }
    // flags raw exposed (visible/action bits) - just check within 0..255
    assert(c.flags <= 0xFF);
  }

  printf("home scene runtime: SCENE parse count/order/iteration PASS (%zu cmds)\n", cmdCount);

  // ---- (1) Action metadata RED tests ----
  {
    // murphy_default: cover with open_current_book (action 0, no arg), text with visible_if $current.exists, text with open_history/open_apps
    bool foundCoverOpenCurrent = false;
    bool foundVisibleCurrentExists = false;
    bool foundOpenHistory = false;
    bool foundOpenApps = false;
    for (size_t i=0;i<cmdCount;++i) {
      auto &c = cmds[i];
      if (c.type == kNodeCover && sceneCommandHasAction(c)) {
        uint8_t aid = sceneCommandActionId(c);
        if (aid == 0) { // open_current_book
          assert(!sceneCommandHasVisibleIf(c));
          assert(!sceneCommandActionHasArg(c));
          assert(sceneCommandActionArgBinding(c) == 0xFF);
          foundCoverOpenCurrent = true;
        }
      }
      if (c.type == kNodeText && sceneCommandHasVisibleIf(c)) {
        uint8_t v = sceneCommandVisibleBinding(c);
        // visible_if 10 = $current.exists per BINDING_SCENE_MAP
        if (v == 10) foundVisibleCurrentExists = true;
        // verify payload helper reads correctly and flags raw is 0x01
        assert((c.flags & kFlagVisibleIf) != 0);
      }
      if (c.type == kNodeText && sceneCommandHasAction(c)) {
        uint8_t aid = sceneCommandActionId(c);
        if (aid == 1) { // open_history
          assert(!sceneCommandActionHasArg(c));
          foundOpenHistory = true;
        }
        if (aid == 2) { // open_apps
          assert(!sceneCommandActionHasArg(c));
          foundOpenApps = true;
        }
      }
    }
    assert(foundCoverOpenCurrent && "cover open_current_book action missing");
    assert(foundVisibleCurrentExists && "visible_if $current.exists missing");
    assert(foundOpenHistory && "open_history action missing");
    assert(foundOpenApps && "open_apps action missing");
    printf("action metadata: cover/open_current_book, visible $current.exists, open_history/open_apps PASS\n");

    // Synthetic open_app + $item.id arg: tests hasArg / arg binding path not present in murphy top-level
    // Payload layout when flags 0x02 and has_arg=1: [action_id, has_arg, arg_binding]
    uint8_t synPayload[] = {3, 1, 30}; // 3=open_app, 1=has_arg, 30=$item.id
    SceneCommand syn{};
    syn.type = kNodeCover;
    syn.flags = kFlagAction;
    syn.payloadLen = 3;
    syn.payload = synPayload;
    syn.offset = 0;
    assert(sceneCommandHasAction(syn));
    assert(sceneCommandActionId(syn) == 3);
    assert(sceneCommandActionHasArg(syn));
    assert(sceneCommandActionArgBinding(syn) == 30);
    assert(!sceneCommandHasVisibleIf(syn));
    // also test combined visible+action with arg
    uint8_t syn2Payload[] = {10, 3, 1, 30}; // visible 10, action 3, has_arg 1, arg 30
    SceneCommand syn2{};
    syn2.type = kNodeText;
    syn2.flags = kFlagVisibleIf | kFlagAction;
    syn2.payloadLen = 4;
    syn2.payload = syn2Payload;
    assert(sceneCommandHasVisibleIf(syn2));
    assert(sceneCommandVisibleBinding(syn2) == 10);
    assert(sceneCommandActionId(syn2) == 3);
    assert(sceneCommandActionHasArg(syn2));
    assert(sceneCommandActionArgBinding(syn2) == 30);
    printf("action metadata: synthetic open_app + $item.id arg PASS\n");
  }

  // ---- (2) Repeat header/source/limit/layout/child boundaries RED tests ----
  {
    // Find both repeat commands: $recent and $apps
    SceneCommand repeats[4];
    size_t repeatCount = 0;
    for (size_t i=0;i<cmdCount;++i) if (cmds[i].type == kNodeRepeat) repeats[repeatCount++] = cmds[i];
    assert(repeatCount == 2 && "murphy_default should have 2 repeats");

    for (size_t ri=0; ri<repeatCount; ++ri) {
      auto &rc = repeats[ri];
      RepeatInfo info{};
      bool pr = parseRepeatInfo(rc, &info);
      assert(pr && "parseRepeatInfo should succeed");
      // Verify source/limit/layout
      if (ri == 0) {
        // first repeat: $recent (binding 20), limit 3, x42 y405 iw130 ih140 gap14 dir0 child2
        assert(info.sourceBinding == 20);
        assert(info.limit == 3);
        assert(info.x == 42 && info.y == 405);
        assert(info.itemW == 130 && info.itemH == 140);
        assert(info.gap == 14);
        assert(info.direction == 0);
        assert(info.childCount == 2);
        printf("repeat $recent header PASS: limit=%u x=%u y=%u iw=%u ih=%u gap=%u dir=%u children=%u\n",
          (unsigned)info.limit,(unsigned)info.x,(unsigned)info.y,(unsigned)info.itemW,(unsigned)info.itemH,(unsigned)info.gap,(unsigned)info.direction,(unsigned)info.childCount);
      } else {
        // second repeat: $apps (21), limit 4, x24 y610 iw107 ih120 gap10 dir0 child2
        assert(info.sourceBinding == 21);
        assert(info.limit == 4);
        assert(info.x == 24 && info.y == 610);
        assert(info.itemW == 107 && info.itemH == 120);
        assert(info.gap == 10);
        assert(info.direction == 0);
        assert(info.childCount == 2);
        printf("repeat $apps header PASS: limit=%u x=%u y=%u iw=%u ih=%u gap=%u dir=%u children=%u\n",
          (unsigned)info.limit,(unsigned)info.x,(unsigned)info.y,(unsigned)info.itemW,(unsigned)info.itemH,(unsigned)info.gap,(unsigned)info.direction,(unsigned)info.childCount);
      }

      // Child command boundaries: iterate children inside repeat payload
      // Must be PROGMEM-safe bounded, no allocation, 4-byte padded, count matches header
      uint16_t childSeen = 0;
      bool cok = forEachRepeatChild(rc, [&](const SceneCommand& child) -> bool {
        // validate child header/padding via pgm_read_byte already done in iterator
        assert(child.payloadLen <= 256);
        // child types should be cover/icon etc.
        childSeen++;
        return true;
      });
      assert(cok && "forEachRepeatChild should succeed");
      assert(childSeen == info.childCount);

      // Also verify first child type matches expected: $recent first child is cover (6), $apps first child is icon (8)
      SceneCommand firstChild{};
      bool gotFirst = false;
      forEachRepeatChild(rc, [&](const SceneCommand& child) -> bool {
        if (!gotFirst) { firstChild = child; gotFirst = true; }
        return true;
      });
      if (ri == 0) assert(firstChild.type == kNodeCover);
      else assert(firstChild.type == kNodeIcon);
    }
    printf("repeat child boundaries PASS\n");

    // Negative: truncated repeat payload should reject
    {
      uint8_t badPayload[5] = {20, 3, 0,0,0}; // too short for repeat header (needs 16)
      SceneCommand bad{}; bad.type=kNodeRepeat; bad.flags=0; bad.payloadLen=5; bad.payload=badPayload;
      RepeatInfo badInfo{};
      assert(!parseRepeatInfo(bad, &badInfo) && "truncated repeat should reject");
      assert(!forEachRepeatChild(bad, [](const SceneCommand&){ return true; }));
    }
  }

  printf("home scene runtime: action+repeat PASS\n");

  // ---- (3) Executor RED: callbacks preserve exact serialized order ----
  {
    uint8_t executed[32]{};
    size_t executedCount = 0;
    bool executedOk = executeScene(data, len, [&](const SceneCommand& command) -> bool {
      assert(executedCount < sizeof(executed));
      executed[executedCount++] = command.type;
      return true;
    });
    assert(executedOk);
    assert(executedCount == sizeof(expected));
    for (size_t i = 0; i < executedCount; ++i) {
      assert(executed[i] == expected[i] && "executor must preserve serialized command order");
    }
  }

  return 0;
}
