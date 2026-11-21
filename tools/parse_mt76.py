#!/usr/bin/env python3
"""Parse an MT7601U Andes firmware image (32-byte header + ILM/DLM)."""
import argparse
import struct
from pathlib import Path

HDR_SIZE = 32
IVB_SIZE = 0x40
DLM_OFFSET = 0x80000


def parse_header(data: bytes) -> dict:
    if len(data) < HDR_SIZE:
        raise ValueError("file shorter than 32-byte header")
    ilm_len, dlm_len, fw_ver, build_ver = struct.unpack_from("<IIHH", data, 0)
    extra = struct.unpack_from("<I", data, 12)[0]
    build_time = data[16:32].split(b"\x00", 1)[0].decode("ascii", "replace")
    expected = HDR_SIZE + ilm_len + dlm_len
    return {
        "ilm_len": ilm_len,
        "dlm_len": dlm_len,
        "fw_ver_raw": fw_ver,
        "fw_ver": f"{(fw_ver >> 12) & 0xF}.{(fw_ver >> 8) & 0xF}.{fw_ver & 0xF:02d}",
        "build_ver": build_ver,
        "extra": extra,
        "build_time": build_time,
        "expected_size": expected,
        "file_size": len(data),
        "ivb_size": IVB_SIZE,
        "ilm_load_addr": IVB_SIZE,
        "dlm_load_addr": DLM_OFFSET,
    }


def split_sections(data: bytes, info: dict) -> dict:
    off = HDR_SIZE
    ilm = data[off : off + info["ilm_len"]]
    off += info["ilm_len"]
    dlm = data[off : off + info["dlm_len"]]
    ivb = ilm[:IVB_SIZE]
    ilm_body = ilm[IVB_SIZE:]
    return {"ivb": ivb, "ilm": ilm_body, "ilm_full": ilm, "dlm": dlm}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("-o", "--outdir", type=Path, default=None)
    args = ap.parse_args()
    data = Path(args.firmware).read_bytes()
    info = parse_header(data)
    for k, v in info.items():
        if isinstance(v, int) and k not in ("file_size", "expected_size", "ivb_size"):
            print(f"{k:16s} 0x{v:08x} ({v})")
        else:
            print(f"{k:16s} {v}")
    if info["expected_size"] != info["file_size"]:
        print("WARNING: size mismatch")
    secs = split_sections(data, info)
    outdir = args.outdir
    if outdir:
        outdir.mkdir(parents=True, exist_ok=True)
        stem = Path(args.firmware).stem
        for name in ("ivb", "ilm", "ilm_full", "dlm"):
            path = outdir / f"{stem}_{name}.bin"
            path.write_bytes(secs[name])
            print(f"wrote {path} {len(secs[name])} bytes")


if __name__ == "__main__":
    main()
