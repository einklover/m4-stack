# PlatformIO pre-build check: the embedded native-grid blob must match the
# generated header/manifest identity. Regeneration still requires the external
# source TTF via firmware/scripts/generate_m4_native_grid.py --font <ttf>.

Import("env")

import subprocess
import sys
from pathlib import Path

script = Path(env["PROJECT_DIR"]) / "scripts/generate_m4_native_grid.py"
result = subprocess.run([sys.executable, str(script), "--verify"], check=False)
if result.returncode != 0:
    sys.exit(result.returncode)
