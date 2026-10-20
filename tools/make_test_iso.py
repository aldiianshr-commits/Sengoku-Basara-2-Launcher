#!/usr/bin/env python3
"""Create a minimal PS2-style ISO9660 disc image (single root directory) for
tests: SYSTEM.CNF + a boot ELF + optional extra files.

usage: make_test_iso.py OUT.iso --elf PROGRAM.elf --boot-name HBRW_000.01
                        [--volume NAME] [--file NAME=PATH ...]

Only legal homebrew test programs are packed; no game data is involved.
"""
import argparse
import struct
import time
from pathlib import Path

SECTOR = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
    t = time.gmtime(0)
    date = bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])
    n = len(name)
    length = 33 + n + (1 if n % 2 == 0 else 0)
    rec = bytes([length, 0]) + both32(lba) + both32(size) + date + bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([n]) + name
    if n % 2 == 0:
        rec += b"\0"
    return rec


def make_iso(out: Path, files, volume: str):
    # Layout: 0-15 system area, 16 PVD, 17 terminator, 18 L path table,
    # 19 M path table, 20 root directory, 21.. file data.
    root_lba = 20
    lba = 21
    entries = []
    for name, data in files:
        entries.append((name, lba, data))
        lba += (len(data) + SECTOR - 1) // SECTOR
    total = lba
    root = dir_record(b"\0", root_lba, SECTOR, True) + dir_record(b"\1", root_lba, SECTOR, True)
    for name, flba, data in sorted(entries, key=lambda e: e[0]):
        root += dir_record((name.upper() + ";1").encode(), flba, len(data), False)
    assert len(root) <= SECTOR
    root = root.ljust(SECTOR, b"\0")
    pt_l = bytes([1, 0]) + struct.pack("<I", root_lba) + struct.pack("<H", 1) + b"\0\0"
    pt_m = bytes([1, 0]) + struct.pack(">I", root_lba) + struct.pack(">H", 1) + b"\0\0"
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[40:72] = volume.upper().encode().ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(len(pt_l))
    pvd[140:144] = struct.pack("<I", 18)
    pvd[148:152] = struct.pack(">I", 19)
    pvd[156:190] = dir_record(b"\0", root_lba, SECTOR, True)
    pvd[881] = 1
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    img = bytearray(SECTOR * total)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:18 * SECTOR] = term
    img[18 * SECTOR:18 * SECTOR + len(pt_l)] = pt_l
    img[19 * SECTOR:19 * SECTOR + len(pt_m)] = pt_m
    img[root_lba * SECTOR:(root_lba + 1) * SECTOR] = root
    for name, flba, data in entries:
        img[flba * SECTOR:flba * SECTOR + len(data)] = data
    out.write_bytes(bytes(img))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--elf", type=Path, required=True)
    ap.add_argument("--boot-name", default="HBRW_000.01")
    ap.add_argument("--volume", default="HOMEBREW")
    ap.add_argument("--file", action="append", default=[])
    a = ap.parse_args()
    cnf = f"BOOT2 = cdrom0:\\{a.boot_name};1\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode()
    files = [("SYSTEM.CNF", cnf), (a.boot_name, a.elf.read_bytes())]
    for f in a.file:
        name, path = f.split("=", 1)
        files.append((name, Path(path).read_bytes()))
    make_iso(a.out, files, a.volume)
    print(f"wrote {a.out} ({a.out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
