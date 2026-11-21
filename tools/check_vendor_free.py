#!/usr/bin/env python3
"""Reject unresolved or explicitly stock-dependent firmware artifacts."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parse_mt76 import parse_header  # noqa: E402

FORBIDDEN_NAMES = ("stock", "vendor", "FUN_", "cmd_fun_set_tssi")
ILM_BASE = 0x00000000
ILM_LIMIT = 0x10000
DLM_BASE = 0x00080000
DLM_LIMIT = 0x8000


def elf_endian(elf: bytes) -> str:
    if len(elf) < 52 or elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] not in (1, 2):
        raise ValueError("expected ELF32 with a defined data byte order")
    return "<" if elf[5] == 1 else ">"


def elf_header(elf: bytes) -> tuple[int, int, int, int, int]:
    endian = elf_endian(elf)
    entry = struct.unpack_from(endian + "I", elf, 24)[0]
    shoff = struct.unpack_from(endian + "I", elf, 32)[0]
    entsz = struct.unpack_from(endian + "H", elf, 46)[0]
    count = struct.unpack_from(endian + "H", elf, 48)[0]
    shstrndx = struct.unpack_from(endian + "H", elf, 50)[0]
    return entry, shoff, entsz, count, shstrndx


def alloc_sections(elf: bytes) -> list[tuple[str, int, int, int]]:
    endian = elf_endian(elf)
    _entry, shoff, entsz, count, shstrndx = elf_header(elf)
    str_sh = shoff + shstrndx * entsz
    str_off, str_size = struct.unpack_from(endian + "II", elf, str_sh + 16)
    names = elf[str_off:str_off + str_size]
    result = []
    for index in range(count):
        sh = shoff + index * entsz
        name_off, sh_type, flags = struct.unpack_from(endian + "III", elf, sh)
        addr, _off, size = struct.unpack_from(endian + "III", elf, sh + 12)
        if not (flags & 2) or not size:  # SHF_ALLOC
            continue
        name = names[name_off:].split(b"\0", 1)[0].decode("ascii", "replace")
        result.append((name, addr, size, sh_type))
    return result


def symbols(elf: bytes) -> list[tuple[str, int, int]]:
    endian = elf_endian(elf)
    shoff = struct.unpack_from(endian + "I", elf, 32)[0]
    entsz = struct.unpack_from(endian + "H", elf, 46)[0]
    count = struct.unpack_from(endian + "H", elf, 48)[0]
    result = []
    for i in range(count):
        sh = shoff + i * entsz
        name, sh_type = struct.unpack_from(endian + "II", elf, sh)
        off, size, link = struct.unpack_from(endian + "III", elf, sh + 16)
        ent_size = struct.unpack_from(endian + "I", elf, sh + 36)[0]
        if sh_type != 2 or not ent_size:  # SHT_SYMTAB
            continue
        str_sh = shoff + link * entsz
        str_off, str_size = struct.unpack_from(endian + "II", elf, str_sh + 16)
        strings = elf[str_off:str_off + str_size]
        for pos in range(off, off + size, ent_size):
            st_name, value, _st_size, info, _other, shndx = struct.unpack_from(endian + "IIIBBH", elf, pos)
            text = strings[st_name:].split(b"\0", 1)[0].decode("ascii", "replace")
            result.append((text, value, shndx))
    return result


def check_elf(path: Path) -> list[str]:
    elf = path.read_bytes()
    errors = []
    entry, _shoff, _entsz, _count, _shstrndx = elf_header(elf)
    if entry != 0xA4:
        errors.append(f"unexpected ELF entry point: 0x{entry:x} (expected 0xa4)")
    for section, addr, size, _sh_type in alloc_sections(elf):
        ilm = ILM_BASE <= addr and addr + size <= ILM_LIMIT
        dlm = DLM_BASE <= addr and addr + size <= DLM_BASE + DLM_LIMIT
        if not (ilm or dlm):
            errors.append(
                f"allocated section {section} escapes ILM/DLM: "
                f"0x{addr:x}+0x{size:x}")
    for name, value, shndx in symbols(elf):
        if shndx == 0 and name and not name.startswith("_GLOBAL_OFFSET_TABLE_"):
            errors.append(f"undefined symbol: {name}")
        if any(word.lower() in name.lower() for word in FORBIDDEN_NAMES):
            errors.append(f"stock-dependent symbol: {name} @ 0x{value:x}")
    return errors


def check_image(path: Path) -> list[str]:
    data = path.read_bytes()
    info = parse_header(data)
    errors = []
    if info["ilm_len"] > 0x10000:
        errors.append(f"ILM exceeds 64 KiB: {info['ilm_len']}")
    if info["dlm_len"] > 0x8000:
        errors.append(f"DLM exceeds 32 KiB: {info['dlm_len']}")
    if info["expected_size"] != info["file_size"]:
        errors.append("header length does not match image size")
    ivb = data[32:36]
    if ivb != bytes.fromhex("48000052"):
        errors.append(f"unexpected IVB reset word: {ivb.hex()}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", type=Path)
    ap.add_argument("--image", type=Path)
    args = ap.parse_args()
    if not args.elf and not args.image:
        ap.error("at least one of --elf or --image is required")
    errors = []
    if args.elf:
        errors.extend(check_elf(args.elf))
    if args.image:
        errors.extend(check_image(args.image))
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("vendor-free checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
