#!/usr/bin/env python3
"""End-to-end tests through the user-facing pipeline (sb2build): every test
disc image is recompiled, compiled with the configured toolchain (the bundled
clang on Windows), linked and run headless; console output is compared with
the expectation file.  Emits GitHub Actions annotations with the results.

usage: run_iso_tests.py --sb2build PATH --isos DIR --expected DIR [--frames-map name=N,...]
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

EXTRA = {"pad": ["--", "--test-pad", "4010"]}
FRAMES = {"threads": 240, "sound": 200, "mpeg": 600}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sb2build", required=True)
    ap.add_argument("--isos", type=Path, required=True)
    ap.add_argument("--expected", type=Path, required=True)
    ap.add_argument("--log", type=Path, default=Path("iso-tests.log"))
    a = ap.parse_args()
    gha = os.environ.get("GITHUB_ACTIONS") == "true"
    results = []
    log = []
    for iso in sorted(a.isos.glob("*.iso")):
        name = iso.stem
        exp_file = a.expected / f"{name}.txt"
        if not exp_file.exists():
            continue
        expected = exp_file.read_text().splitlines()
        b = subprocess.run([a.sb2build, str(iso)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1800)
        log.append(f"===== {name}: build (exit {b.returncode})\n{b.stdout[-4000:]}")
        if b.returncode != 0:
            results.append((name, False, "build failed"))
            continue
        cmd = [a.sb2build, str(iso), "--play", "--headless", "--frames", str(FRAMES.get(name, 120)), *EXTRA.get(name, [])]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
        out = r.stdout.splitlines()
        log.append(f"===== {name}: run (exit {r.returncode})\n{r.stdout[-6000:]}")
        pos, missing = 0, []
        for line in expected:
            try:
                pos = out.index(line, pos) + 1
            except ValueError:
                missing.append(line)
        ok = not missing and r.returncode == 0
        results.append((name, ok, "" if ok else f"exit {r.returncode}; missing: {missing[:3]}"))
    a.log.write_text("\n".join(log))
    passed = sum(1 for _, ok, _ in results if ok)
    for name, ok, why in results:
        print(f"{'PASS' if ok else 'FAIL'} {name} {why}")
    summary = f"{passed}/{len(results)} pipeline tests passed: " + ", ".join(f"{n}={'ok' if ok else 'FAIL'}" for n, ok, _ in results)
    print(summary)
    if gha:
        print(f"::notice title=Pipeline tests ({sys.platform})::{summary}")
        for name, ok, why in results:
            if not ok:
                print(f"::error title=Pipeline test {name}::{why}")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
