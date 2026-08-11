#include <cstdio>

#include "core/SimKernel.h"
#include "model/ReaderModel.h"
#include "native/NativePorts.h"

int main() {
  m4sim::SimScheduler sched(1);
  m4sim::SimTrace trace;
  m4native::NativeDisplay display(&sched);
  m4native::NativeStorage storage(&sched);

  m4sim::ReaderModel reader(
      &sched, &trace, &display, &storage,
      m4sim::ReaderModel::Knobs{/*bugNoCatchup=*/false,
                                /*bugLivePhysical=*/false,
                                /*sdScale=*/1.0,
                                /*indexSlicePages=*/64,
                                /*animEnabled=*/false});

  reader.openBook(8);
  sched.runFor(200);
  if (!reader.firstShown() || reader.physicalPage() != 0 ||
      !display.hasPhysicalFrame() || display.physicalTag().page != 0) {
    std::fprintf(stderr, "native smoke: failed to commit initial page\n");
    return 1;
  }

  reader.tap(+1);
  sched.runFor(200);
  if (reader.physicalPage() != 1 || display.physicalTag().page != 1) {
    std::fprintf(stderr, "native smoke: failed to commit page 1\n");
    return 2;
  }

  std::printf("native smoke PASS: ReaderModel runs on non-SimPanel/non-SimStorage ports\n");
  return 0;
}
