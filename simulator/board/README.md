# Board specifications

`murphy_m4.json` is the machine-readable board truth used by validation and
future QEMU-device generation. `MurphyM4Spec.h` is the dependency-free C++ view
used by deterministic low-level tests.

The JSON deliberately distinguishes:

- live-probed/firmware-authoritative values,
- schematic-only physical devices,
- firmware-disabled capabilities,
- unresolved source discrepancies.

Do not copy pin numbers into new simulator code. Consume `MurphyM4Spec.h` or the
JSON so changes remain reviewable and CI can detect drift.
