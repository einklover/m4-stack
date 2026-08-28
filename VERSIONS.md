# Murphy M4 pinned sources

Updated: **2026-08-28**

| Input | Source / scope | Pin or baseline |
|---|---|---|
| Monorepo baseline | `einklover/m4-stack` `main` | `faca95d6618791d5e10295a608c484b2518ba5ea` |
| Murphy M4 SDK | vendored FreeInk SDK subset | `m4-crosspoint-native/freeink-sdk@7c4aac5b1a8a2a159c3738081aa5bcdaf0a7aec0` |
| Lua | vendored upstream source | `5.4.7` |
| Expat | vendored upstream source | `2.7.3` |
| miniz | vendored upstream source | `2.2.0` |
| picojpeg | vendored upstream source | public-domain source, version not declared upstream |
| Epub | vendored m4-stack firmware source | firmware `LICENSE` |
| M4 PlatformIO platform | `pioarduino/platform-espressif32` release archive | `55.03.37` |
| Legacy PlatformIO base | `espressif32` | `6.12.0` |
| Patched QEMU source | `espressif/qemu` | `febae182e132e4055529be423a818225ebddaa3a` |
| Murphy QEMU patch set | `simulator/qemu/patches/series-v3` | repository-controlled ordered series |

The M4 build does not require the former private device checkout or any sibling repository.
`firmware/lib/EpdFont/builtinFonts/` is generated from a builder-supplied TTF
and remains ignored; see [Build and dependencies](docs/BUILD_AND_DEPS.md).
