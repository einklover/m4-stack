#include <cstddef>
#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "core/PageTurnCoordinator.h"

namespace {
constexpr const char* kTag = "m4-qemu";
}

extern "C" void app_main(void) {
  // Shared production logic smoke: this exact coordinator header is also used
  // by the deterministic/native targets. If it stops being ESP-IDF portable,
  // this target fails at compile time before a real M4 is flashed.
  m4reader::PageTurnCoordinator coord(m4reader::PageTurnCoordinator::productionPolicy());
  coord.reset();
  coord.onTap(/*nowMs=*/100, /*panelBusy=*/false, /*newTarget=*/1);
  if (!coord.shouldRender(/*panelBusy=*/false, /*suppressDisplay=*/false)) {
    ESP_LOGE(kTag, "PageTurnCoordinator did not arm page 1");
    std::abort();
  }
  coord.onRenderStarted();
  int renderedPage = coord.onFrameReady();
  coord.onCommitted(/*nowMs=*/120, renderedPage);
  if (coord.physicalPage() != 1) {
    ESP_LOGE(kTag, "PageTurnCoordinator physical page mismatch: %d", coord.physicalPage());
    std::abort();
  }

  size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internalLargest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  ESP_LOGI(kTag, "shared coordinator PASS");
  ESP_LOGI(kTag, "internal free=%u largest=%u", (unsigned)internalFree,
           (unsigned)internalLargest);
  ESP_LOGI(kTag, "psram total=%u free=%u", (unsigned)psramTotal, (unsigned)psramFree);

  // Optional PSRAM capability smoke. QEMU can run without PSRAM enabled in the
  // project config, so absence is reported rather than treated as a failure.
  if (psramTotal != 0) {
    void* fb = heap_caps_malloc(48000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
      ESP_LOGE(kTag, "48KB PSRAM framebuffer allocation failed");
      std::abort();
    }
    ESP_LOGI(kTag, "48KB PSRAM framebuffer allocation PASS");
    heap_caps_free(fb);
  } else {
    ESP_LOGW(kTag, "PSRAM not enabled in this QEMU project configuration");
  }
}
