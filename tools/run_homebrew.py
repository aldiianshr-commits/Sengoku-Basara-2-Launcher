#!/usr/bin/env python3
"""Build the homebrew test programs in tests/homebrew with clang (MIPS) and
ld.lld, statically recompile them, run them headless and check the rendered
frame and console output against *.expect.json.

usage: run_homebrew.py --build BUILD_DIR [--cxx g++]
"""
import argparse, json, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFLAGS = ["--target=mips64el-unknown-none-elf", "-mcpu=mips3", "-mabi=n32", "-msingle-float", "-O2", "-G0",
          "-mno-abicalls", "-fno-pic", "-ffreestanding", "-fno-builtin", "-nostdlib"]


def sh(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, cmd))}\n{r.stdout}\n{r.stderr}")
    return r


def read_ppm(path):
    d = path.read_bytes()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--lld", default="ld.lld")
    args = ap.parse_args()
    src = ROOT / "tests" / "homebrew"
    work = args.build / "homebrew"
    work.mkdir(parents=True, exist_ok=True)
    sh([args.clang, *CFLAGS, "-c", str(src / "crt0.S"), "-o", str(work / "crt0.o")])
    failures = 0
    for exp in sorted(src.glob("*.expect.json")):
        name = exp.name.replace(".expect.json", "")
        spec = json.loads(exp.read_text())
        obj, elf = work / f"{name}.o", work / f"{name}.elf"
        sh([args.clang, *CFLAGS, "-I", str(src), "-c", str(src / f"{name}.c"), "-o", str(obj)])
        sh([args.lld, "-m", "elf32ltsmipn32", "-T", str(src / "link.ld"), str(work / "crt0.o"), str(obj), "-o", str(elf)])
        gen = work / "gen" / name
        sh([str(args.build / "ps2recomp"), "--elf", str(elf), "--out", str(gen), "--name", name])
        objs = []
        for c in sorted(gen.glob("*.cpp")):
            o = c.with_suffix(".o")
            sh([args.cxx, "-std=c++20", "-O1", "-fno-strict-aliasing", "-w", "-I", str(ROOT / "runtime" / "include"), "-c", str(c), "-o", str(o)])
            objs.append(str(o))
        exe = work / f"{name}.exe"
        sh([args.cxx, "-o", str(exe), *objs, str(args.build / "libps2main.a"), str(args.build / "libps2runtime.a"),
            str(args.build / "libsb2common.a"), "-lpthread"])
        ppm = work / f"{name}.ppm"
        r = subprocess.run([str(exe), "--elf", str(elf), "--headless", "--dump-frame", str(ppm)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300)
        ok = True
        out_lines = r.stdout.splitlines()
        for line in spec.get("console", []):
            if line not in out_lines:
                print(f"  [{name}] missing console line: {line!r}")
                ok = False
        if spec.get("pixels"):
            w, h, px = read_ppm(ppm)
            for p in spec["pixels"]:
                i = (p["y"] * w + p["x"]) * 3
                got = list(px[i:i + 3])
                tol = p.get("tol", 0)
                if any(abs(a - b) > tol for a, b in zip(got, p["rgb"])):
                    print(f"  [{name}] {p['name']}: expected {p['rgb']} got {got}")
                    ok = False
        print(f"{'PASS' if ok else 'FAIL'} {name}")
        failures += 0 if ok else 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
