"""Select the compiled inspector shared by the native regression suites."""
import os
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
configured = os.environ.get("YSM_TEST_INSPECTOR")
if configured:
    EXE = Path(configured).resolve(strict=True)
else:
    EXE = PROJECT / "build/windows/x64/release/ysm-inspect.exe"
    if not EXE.is_file():
        EXE = PROJECT.parent / "ysm-inspect.exe"
