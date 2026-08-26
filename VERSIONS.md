# Murphy M4 pinned sources

Updated: **2026-08-26**

| Input | Source / scope | Pin or baseline |
|---|---|---|
| Monorepo baseline | `main-rebuild-20260826` | `bc3038955108eb7573398894eeb63f522b3d0b8b` |
| Reconstructed device SDK and libraries | `einklover/m4-device` archive | `f86b134` |
| M4 PlatformIO platform | `pioarduino/platform-espressif32` release archive | `55.03.37` |
| Legacy PlatformIO base | `espressif32` | `6.12.0` |
| Patched QEMU source | `espressif/qemu` | `febae182e132e4055529be423a818225ebddaa3a` |
| Murphy QEMU patch set | `simulator/qemu/patches/series-v3` | repository-controlled ordered series |

The device archive is fetched only when the automatic M4 bootstrap finds missing sentinels. Generated dependency trees and build outputs remain ignored; see [Build and dependencies](docs/BUILD_AND_DEPS.md).
