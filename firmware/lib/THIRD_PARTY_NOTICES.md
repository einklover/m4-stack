# Vendored firmware dependencies

These sources are copied into the public `m4-stack` tree so a builder does
not need a second repository. Keep the license text in each source file when
updating a component.

| Path | Provenance/version | License evidence |
|---|---|---|
| `open-m4-sdk/` | `m4-crosspoint-native/freeink-sdk@7c4aac5b1a8a2a159c3738081aa5bcdaf0a7aec0` | `open-m4-sdk/LICENSE` and `NOTICE` (MIT) |
| `Lua/` | Lua 5.4.7 | Copyright and MIT notice in `Lua/src/lua.h` |
| `expat/` | Expat 2.7.3 | MIT notices in the upstream source headers |
| `miniz/` | miniz 2.2.0 | Public-domain/unlicense notice in `miniz.h` |
| `picojpeg/` | picojpeg source | Public-domain notice in `picojpeg.h` |
| `Epub/` | m4-stack firmware source | Covered by `firmware/LICENSE` |

`lib/EpdFont/builtinFonts/` is deliberately not listed: it is a generated
production artifact, not a vendored dependency. The generator consumes only
a local font selected by the builder; no source TTF or generated output is
published here.

The former SD-card intermediary updater was intentionally removed. No compiled
second-stage updater image is shipped or required by this repository.
