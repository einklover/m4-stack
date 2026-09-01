# Round 13 — Fanqie tomato / Legado open book

Date: 2026-09-01
Sheet: `docs/orchestration/assets/icon-design-sheet-round9.png` (coord copy; labeled r3c1 番茄 / r3c3 开源阅读)

## Mapping

| id | sheet cell | glyph | crop |
|---|---|---|---|
| `com.fanqie.client` | r3c1 | tomato, caption excluded | [fanqie preview](../assets/round-9-crops/fanqie-preview.png) |
| `com.legado.client` | r3c3 | open book (previous Fanqie BMP) | [legado preview](../assets/round-9-crops/legado-preview.png) |

Tomato glyph box `(132,731,302,896)` exclusive of caption. 62×64 1-bpp BMP, palette 0=black / 1=white, 574 bytes. Tomato black-ink ratio ≈14.4%.

WeRead / JJWXC BMPs unchanged.

## SHA-256

```
fc358337f1e6ba678852dda42a8bb6ec2f8de005f3c4da03be8d93b47668b6c4  plugins/m4-fanqie-plugin/icon_home.bmp
361c2984f36dbe60d9bed8fb018f4850c3493ea0ee4b30d492a1489ae520f966  plugins/m4-legado-plugin/icon_home.bmp
```

`test_plugin_home_icon_resource.py` now includes `m4-legado-plugin` (4 passed).
