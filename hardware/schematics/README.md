# Schematics

## Murphy M4 / ESP32-S3 board schematic

Canonical supplied source as of 2026-08-11:

- Original filename: `SCH_ESP..18(2).pdf`
- Planned repository filename: `Murphy-M4-ESP32-S3R8-ESP32_426_S3_V2.0-2026-05-06.pdf`
- Pages: 2
- Schematic title: `ESP32_S3_6`
- Board field: `ESP32_426_S3_V2.0`
- Revision: `V1.0`
- Created: 2026-03-17
- Updated: 2026-05-06
- Source: supplied directly by the device owner

The PDF is the authoritative drawing; the Markdown files under
`hardware/murphy-m4/` are structured extractions for code review and emulator
implementation. If an extracted fact disagrees with the PDF, correct the
extraction and keep a note in `FACTS.md`.

### Source retention policy

Do not overwrite a schematic silently. If a newer board drawing appears, keep
both revisions when they describe different PCB revisions and add a short note
here describing which shipping hardware each revision matches.

### Binary-source note

The repository automation used for the current staged changes can write text
objects directly. The supplied PDF is also retained in the project file library
and is being mirrored into this directory as a binary Git object in the same
hardware-baseline stage. The structured facts are intentionally committed
separately so emulator work remains reviewable even when the drawing is opened
outside GitHub's text diff UI.
