# Schematics

## Murphy M4 / ESP32-S3 board schematic

Canonical supplied source as of 2026-08-11:

- Original filename: `SCH_ESP..18(2).pdf`
- Intended repository filename: `Murphy-M4-ESP32-S3R8-ESP32_426_S3_V2.0-2026-05-06.pdf`
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

### Binary-source status

The exact PDF is retained in the project file library. The GitHub connector
available to this automation can create UTF-8 repository contents but cannot
accept a local binary file path, so the PDF itself is **not yet committed as a
Git blob**. Do not mistake this Markdown record for the drawing.

Until a normal git/file-upload path is available, hardware facts extracted from
the PDF are committed under `hardware/murphy-m4/` with explicit evidence labels.
When binary upload becomes available, add the exact PDF under the intended name
above and record its SHA-256 here without rewriting or replacing the source.
