#!/usr/bin/env python3
"""Pack each test ELF (tests/ps2sdk/bin, homebrew) into its own disc image."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "tests" / "ps2sdk" / "data"
# extra files per test: disc name -> file in tests/ps2sdk/data
EXTRA_FILES = {"mpeg": {"CLIP2.M2V": "clip2.m2v", "CLIP2.RGB": "clip2.rgb", "CLIP1.M1V": "clip1.m1v", "CLIP1.RGB": "clip1.rgb"}}


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    data = bytes(((i * 7 + 3) & 0xFF) for i in range(40960))
    (out / "DATA.BIN").write_bytes(data)
    for i, elf in enumerate(sorted((ROOT / "tests" / "ps2sdk" / "bin").glob("*.elf"))):
        subprocess.check_call([sys.executable, str(ROOT / "tools" / "make_test_iso.py"), str(out / f"{elf.stem}.iso"), "--elf", str(elf),
                               "--boot-name", f"HBRW_{200 + i:03d}.01", "--volume", elf.stem.upper(), "--file", f"DATA.BIN={out / 'DATA.BIN'}",
                               *[a for k, v in EXTRA_FILES.get(elf.stem, {}).items() for a in ("--file", f"{k}={DATA / v}")]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
