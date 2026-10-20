#!/usr/bin/env python3
"""Run the ps2sdk-based test programs (tests/ps2sdk/bin/*.elf) through the
static recompiler and check their console output against
tests/ps2sdk/expected/<name>.txt (lines must appear in order).

Each program boots from a generated ISO (so cdrom0: works) with a temporary
memory card directory.  Extra checks: the sound test's audio is dumped to a
WAV file and must not be silent.

usage: run_ps2sdk_tests.py --build BUILD_DIR [--cxx g++] [--filter NAME]
"""
import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests" / "ps2sdk"
FRAMES = {"threads": 240, "sound": 200, "mpeg": 600}
EXTRA_ARGS = {"pad": ["--test-pad", "4010"]}
DATA = TESTS / "data"
EXTRA_FILES = {"mpeg": ["CLIP2.M2V=clip2.m2v", "CLIP2.RGB=clip2.rgb", "CLIP1.M1V=clip1.m1v", "CLIP1.RGB=clip1.rgb"]}


def sh(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, cmd))}\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}")
    return r


def wav_rms(path):
    d = Path(path).read_bytes()
    if len(d) < 44:
        return 0.0
    n = (len(d) - 44) // 2
    samples = struct.unpack_from(f"<{n}h", d, 44)
    if not samples:
        return 0.0
    return math.sqrt(sum(s * s for s in samples) / len(samples))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    work = args.build / "ps2sdk_tests"
    work.mkdir(parents=True, exist_ok=True)
    data = bytes(((i * 7 + 3) & 0xFF) for i in range(40960))
    (work / "DATA.BIN").write_bytes(data)
    failures = 0
    for elf in sorted((TESTS / "bin").glob("*.elf")):
        name = elf.stem
        if args.filter and args.filter != name:
            continue
        expected_file = TESTS / "expected" / f"{name}.txt"
        expected = expected_file.read_text().splitlines() if expected_file.exists() else []
        iso = work / f"{name}.iso"
        sh([sys.executable, str(ROOT / "tools" / "make_test_iso.py"), str(iso), "--elf", str(elf), "--boot-name", "HBRW_100.01",
            "--volume", name.upper(), "--file", f"DATA.BIN={work / 'DATA.BIN'}",
            *[a for spec in EXTRA_FILES.get(name, []) for a in ("--file", spec.split("=")[0] + "=" + str(DATA / spec.split("=")[1]))]])
        gen = work / "gen" / name
        if gen.exists():
            shutil.rmtree(gen)
        sh([str(args.build / "ps2recomp"), "--elf", str(elf), "--out", str(gen), "--name", name])
        objs = []
        for c in sorted(gen.glob("*.cpp")):
            o = c.with_suffix(".o")
            sh([args.cxx, "-std=c++20", "-O1", "-fno-strict-aliasing", "-w", "-I", str(ROOT / "runtime" / "include"), "-c", str(c), "-o", str(o)])
            objs.append(str(o))
        exe = work / f"{name}.exe"
        sh([args.cxx, "-o", str(exe), *objs, str(args.build / "libps2main.a"), str(args.build / "libps2runtime.a"),
            str(args.build / "libsb2common.a"), "-lpthread"])
        saves = work / f"{name}_memcards"
        if saves.exists():
            shutil.rmtree(saves)
        wav = work / f"{name}.wav"
        cmd = [str(exe), "--elf", str(elf), "--disc", str(iso), "--boot", "cdrom0:\\HBRW_100.01;1", "--headless",
               "--frames", str(FRAMES.get(name, 120)), "--saves", str(saves), "--dump-audio", str(wav), *EXTRA_ARGS.get(name, [])]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300)
        out = r.stdout.splitlines()
        (work / f"{name}.out").write_text(r.stdout)
        (work / f"{name}.log").write_text(r.stderr)
        ok = True
        pos = 0
        for line in expected:
            try:
                pos = out.index(line, pos) + 1
            except ValueError:
                print(f"  [{name}] missing or out of order: {line!r}")
                ok = False
        if name == "sound":
            rms = wav_rms(wav)
            print(f"  [{name}] audio RMS {rms:.1f}")
            if rms < 200:
                print(f"  [{name}] audio output is silent")
                ok = False
        if not expected:
            print(f"  [{name}] no expectation file; output:\n    " + "\n    ".join(out))
        if args.verbose or not ok:
            print("  stdout:\n    " + "\n    ".join(out[-40:]))
            print("  stderr:\n    " + "\n    ".join(r.stderr.splitlines()[-25:]))
        print(f"{'PASS' if ok else 'FAIL'} {name}")
        failures += 0 if ok else 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
