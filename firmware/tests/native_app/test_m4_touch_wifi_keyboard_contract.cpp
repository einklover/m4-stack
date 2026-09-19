#include <cassert>

#include "../../src/util/TouchUiGeometry.h"

int main() {
  using namespace TouchHitGeometry;

  // 480x800 M4 touch keyboard: three phone-style QWERTY rows plus a control row.
  const auto letters = makeTouchKeyboardLayout(480, 430, 64, 6, TouchKeyboardMode::Letters);
  assert(letters.valid());
  assert(letters.characterRowCount == 3);
  assert(letters.characterCounts[0] == 10);
  assert(letters.characterCounts[1] == 9);
  assert(letters.characterCounts[2] == 7);

  // Letter keys must be finger-sized and the shorter rows must be centered.
  for (int i = 0; i < letters.keyCount; ++i) {
    const auto& key = letters.keys[i];
    if (key.kind == TouchKeyboardKeyKind::Character) {
      assert(key.rect.width >= 40);
      assert(key.rect.height >= 60);
    }
  }
  assert(letters.characterRowBounds[1].x > letters.characterRowBounds[0].x);
  assert(letters.characterRowBounds[2].x > letters.characterRowBounds[1].x);

  // Touch hit testing must match the visible key geometry and leave gaps inert.
  const auto firstKey = letters.keys[0];
  int hitIndex = -1;
  assert(letters.hit(firstKey.rect.x + firstKey.rect.width / 2,
                     firstKey.rect.y + firstKey.rect.height / 2, hitIndex));
  assert(hitIndex == 0);
  hitIndex = -1;
  assert(!letters.hit(firstKey.rect.x + firstKey.rect.width + 2,
                      firstKey.rect.y + firstKey.rect.height / 2, hitIndex));

  // Control keys are large enough for thumb operation and include the essentials.
  const auto* mode = letters.find(TouchKeyboardKeyKind::Mode);
  const auto* shift = letters.find(TouchKeyboardKeyKind::Shift);
  const auto* space = letters.find(TouchKeyboardKeyKind::Space);
  const auto* backspace = letters.find(TouchKeyboardKeyKind::Backspace);
  const auto* confirm = letters.find(TouchKeyboardKeyKind::Confirm);
  assert(mode && shift && space && backspace && confirm);
  assert(mode->rect.height >= 60);
  assert(space->rect.width >= 120);
  assert(confirm->rect.width >= 70);

  // Symbol mode must expose digits and punctuation without returning to a 13-column grid.
  const auto symbols = makeTouchKeyboardLayout(480, 430, 64, 6, TouchKeyboardMode::Symbols);
  assert(symbols.valid());
  assert(symbols.characterRowCount >= 3);
  assert(symbols.maxCharactersPerRow <= 10);
  assert(symbols.containsCharacter('0'));
  assert(symbols.containsCharacter('@'));
  assert(symbols.containsCharacter('/'));
  assert(symbols.containsCharacter('\\'));

  // Keyboard interaction state is touch-oriented: one-shot shift and direct mode toggle.
  TouchKeyboardState state;
  assert(state.mode == TouchKeyboardMode::Letters);
  assert(!state.shifted);
  state.toggleShift();
  assert(state.shifted);
  assert(state.resolveCharacter('q') == 'Q');
  assert(!state.shifted);  // one-shot shift clears after a letter
  state.toggleMode();
  assert(state.mode == TouchKeyboardMode::Symbols);
  assert(!state.shifted);
  assert(state.resolveCharacter('@') == '@');
  state.toggleMode();
  assert(state.mode == TouchKeyboardMode::Letters);

  // Wi-Fi list rows must be large, full-width touch targets with a dedicated refresh target.
  const auto wifi = makeWifiNetworkListLayout(480, 800, 7);
  assert(wifi.valid());
  assert(wifi.rowHeight >= 64);
  assert(wifi.rowWidth >= 440);
  assert(wifi.refresh.width >= 80);
  assert(wifi.refresh.height >= 52);

  int networkIndex = -1;
  const auto row0 = wifi.rowRect(0);
  assert(wifi.hitRow(row0.x + row0.width / 2, row0.y + row0.height / 2, 7, networkIndex));
  assert(networkIndex == 0);

  // Spacing between rows must not activate either neighbour.
  networkIndex = -1;
  assert(!wifi.hitRow(row0.x + row0.width / 2, row0.y + row0.height + 1, 7, networkIndex));

  return 0;
}
