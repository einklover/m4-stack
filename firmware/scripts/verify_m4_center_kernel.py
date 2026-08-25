# PlatformIO pre-build check: the embedded CenterKernel blob must match the
# generated header/manifest identity.

Import("env")

import subprocess
import sys
from pathlib import Path

script = Path(env["PROJECT_DIR"]) / "scripts/generate_m4_center_kernel.py"
result = subprocess.run([sys.executable, str(script), "--verify"], check=False)
if result.returncode != 0:
    sys.exit(result.returncode)
