#!/usr/bin/env python3
"""Pack a vendor-free MT7601U ELF with separate ILM and DLM sections."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_vendor_free import elf_endian
from pack_mt76 import build, utc_build_time  # noqa: E402

ILM_BASE = 0x00000000
ILM_LIMIT = 0x10000
DLM_BASE = 0x00080000
DLM_LIMIT = 0x8000
VERSION_RECORD = b"Vers\xb1\x00\x00\x00"


def sections(elf: bytes) -> list[tuple[bytes, int, int, int, bytes]]:
    endian = elf_endian(elf)
    e_shoff = struct.unpack_from(endian + "I", elf, 32)[0]
    e_shentsize = struct.unpack_from(endian + "H", elf, 46)[0]
    e_shnum = struct.unpack_from(endian + "H", elf, 48)[0]
    e_shstrndx = struct.unpack_from(endian + "H", elf, 50)[0]
    str_sh = e_shoff + e_shstrndx * e_shentsize
    str_off, str_size = struct.unpack_from(endian + "II", elf, str_sh + 16)
    names = elf[str_off:str_off + str_size]
    out = []
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        name_off, sh_type, flags = struct.unpack_from(endian + "III", elf, sh)
        addr, off, size = struct.unpack_from(endian + "III", elf, sh + 12)
        if not (flags & 2) or not size:  # SHF_ALLOC
            continue
        name = names[name_off:].split(b"\0", 1)[0]
        data = b"" if sh_type == 8 else elf[off:off + size]
        out.append((name, addr, size, sh_type, data))
    return out


def region_image(elf: bytes, base: int, limit: int) -> bytes:
    entries = []
    end = base
    for name, addr, size, sh_type, data in sections(elf):
        if addr + size <= base or addr >= base + limit:
            continue
        if addr < base or addr + size > base + limit:
            raise ValueError(f"section {name!r} crosses region boundary")
        if sh_type not in (1, 8):  # PROGBITS or NOBITS
            continue
        entries.append((addr, size, sh_type, data))
        if sh_type == 1:
            end = max(end, addr + size)
    image = bytearray(end - base)
    for addr, size, sh_type, data in entries:
        if sh_type == 1:
            image[addr - base:addr - base + size] = data
    return bytes(image)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--output", "-o", required=True, type=Path)
    ap.add_argument("--build-time", default=None)
    args = ap.parse_args()
    raw = args.elf.read_bytes()
    ilm = region_image(raw, ILM_BASE, ILM_LIMIT)
    dlm = region_image(raw, DLM_BASE, DLM_LIMIT)
    if len(ilm) < 0xA4:
        raise SystemExit("ILM does not contain the required entry address")
    # The footer owns the version of linked firmware. Preserve the default
    # header version for older ELFs that have no version record.
    version = (struct.unpack_from("<HH", ilm, len(ilm) - 4)
               if ilm[-12:-4] == VERSION_RECORD else (0x0001, 0x0001))
    build_time = args.build_time
    if build_time is None:
        try:
            build_time = utc_build_time(args.elf.stat().st_mtime_ns // 1_000_000_000)
        except (ValueError, OverflowError, OSError) as error:
            ap.error(f"invalid build timestamp: {error}")
    image = build(ilm, dlm, build_time=build_time, version=version)
    args.output.write_bytes(image)
    print(f"wrote {args.output} {len(image)} bytes (ILM {len(ilm)}, DLM {len(dlm)})")


if __name__ == "__main__":
    main()
