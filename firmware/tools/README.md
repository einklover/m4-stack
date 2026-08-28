# M4 UI preview tools

These host-only tools preview the current 480×800 Murphy M4 Native UI. They
do not participate in the firmware build and make no network requests.

From the repository root on macOS or Linux:

```bash
python3 firmware/tools/m4ui_preview.py --screen both --gallery
python3 firmware/tools/m4ui_pixel_html.py --screen both
```

Output defaults to `firmware/tools/preview_output/`, which is ignored by Git.
Use `--out PATH` for another generated-output directory. Both tools reject
paths under `firmware/src/`. The PNG renderer discovers a common system font;
use `--font PATH` and optionally `--bold-font PATH` for an explicit TTF/TTC/OTF
override. `--help` works without Pillow or any font installed.

Preview assets should be generated locally or come from repository-owned,
licensed sources. The tools intentionally do not bundle font files or
third-party icon artwork.
