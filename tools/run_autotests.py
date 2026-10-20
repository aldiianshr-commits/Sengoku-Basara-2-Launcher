#!/usr/bin/env python3
"""Recompile, build and run the ps2autotests suite and compare each program's
output with the result recorded on real PS2 hardware (*.expected).

usage: run_autotests.py --suite PATH/ps2autotests --build BUILD_DIR [--filter cpu/ee] [--jobs N]
"""
import argparse
import concurrent.futures as cf
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)


HEADER_MTIME = 0.0


def compile_one(cxx, src, obj, include, flags):
    if obj.exists() and obj.stat().st_mtime >= max(src.stat().st_mtime, HEADER_MTIME):
        return True, ""
    r = run([cxx, "-std=c++20", *flags, "-fno-strict-aliasing", "-w", "-I", str(include), "-c", str(src), "-o", str(obj)])
    return r.returncode == 0, r.stderr


def build_test(name, elf, args, work):
    gen = work / "gen" / name
    gen.mkdir(parents=True, exist_ok=True)
    r = run([str(args.build / "ps2recomp"), "--elf", str(elf), "--out", str(gen), "--name", name.replace("/", "_")])
    if r.returncode != 0:
        return None, "recompile failed: " + r.stderr
    srcs = sorted(gen.glob("*.cpp"))
    objs = []
    for s in srcs:
        o = s.with_suffix(".o")
        ok, err = compile_one(args.cxx, s, o, args.root / "runtime" / "include", args.flags)
        if not ok:
            return None, "compile failed: " + err[-2000:]
        objs.append(o)
    exe = work / "bin" / (name.replace("/", "_") + ".exe")
    exe.parent.mkdir(parents=True, exist_ok=True)
    libs = [args.build / "libps2main.a", args.build / "libps2runtime.a", args.build / "libsb2common.a"]
    r = run([args.cxx, "-o", str(exe), *map(str, objs), *map(str, libs), "-lpthread"])
    if r.returncode != 0:
        return None, "link failed: " + r.stderr[-2000:]
    return exe, ""


def run_test(t, args, work):
    name, elf, expected = t
    t0 = time.time()
    exe, err = build_test(name, elf, args, work)
    if not exe:
        return name, "BUILD-FAIL", err, 0.0, ""
    try:
        r = run([str(exe), "--elf", str(elf), "--headless", "--max-cycles", str(args.max_cycles)], timeout=args.timeout)
        out = r.stdout
        log = r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        log = "TIMEOUT"
    exp = expected.read_text(errors="replace")
    (work / "out").mkdir(exist_ok=True)
    (work / "out" / (name.replace("/", "_") + ".out")).write_text(out)
    (work / "out" / (name.replace("/", "_") + ".log")).write_text(log)
    exp_lines = exp.replace("\r\n", "\n").splitlines()
    out_lines = out.replace("\r\n", "\n").splitlines()
    if out_lines == exp_lines:
        status = "PASS"
    else:
        same = sum(1 for a, b in zip(out_lines, exp_lines) if a == b)
        status = f"FAIL ({same}/{len(exp_lines)} lines match)"
    return name, status, log[-1500:], time.time() - t0, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", type=Path, required=True)
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--filter", default="")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--flags", default="-O1")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--max-cycles", type=int, default=20_000_000_000)
    ap.add_argument("--json", type=Path, default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    args.flags = args.flags.split()
    global HEADER_MTIME
    HEADER_MTIME = max(p.stat().st_mtime for p in (args.root / "runtime" / "include").rglob("*.hpp"))
    work = args.work or (args.build / "autotests")
    work.mkdir(parents=True, exist_ok=True)
    tests = []
    for exp in sorted((args.suite / "tests").rglob("*.expected")):
        elf = exp.with_suffix(".elf")
        if not elf.exists():
            continue
        name = str(exp.relative_to(args.suite / "tests").with_suffix("")).replace("\\", "/")
        if args.filter and not name.startswith(args.filter):
            continue
        tests.append((name, elf, exp))
    results = []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(run_test, t, args, work) for t in tests]
        for f in cf.as_completed(futs):
            name, status, log, dt, _ = f.result()
            results.append((name, status, dt))
            print(f"{status:30s} {name}  ({dt:.1f}s)", flush=True)
            if args.verbose and status != "PASS":
                print("   " + log.replace("\n", "\n   "))
    passed = sum(1 for r in results if r[1] == "PASS")
    print(f"\n{passed}/{len(results)} tests match real PS2 hardware output")
    if args.json:
        args.json.write_text(json.dumps([{"test": n, "status": s, "seconds": round(d, 2)} for n, s, d in sorted(results)], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
