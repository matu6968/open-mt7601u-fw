#!/usr/bin/env python3
"""Pack an MT7601U firmware image (32-byte header + ILM including IVB + DLM)."""
import argparse
from datetime import datetime, timezone
import os
import struct
from pathlib import Path

HDR_SIZE = 32
IVB_SIZE = 0x40


def utc_build_time(link_epoch: int | None = None) -> str:
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is not None:
        link_epoch = int(epoch)
        if link_epoch < 0:
            raise ValueError("SOURCE_DATE_EPOCH must be nonnegative")
    timestamp = (datetime.now(timezone.utc) if link_epoch is None else
                 datetime.fromtimestamp(link_epoch, timezone.utc))
    return timestamp.strftime("%Y%m%d%H%M____")


def build(ilm: bytes, dlm: bytes = b"", build_time=None,
          template: bytes | None = None, *, version=(0x0001, 0x0001)) -> bytes:
    if len(ilm) < IVB_SIZE:
        ilm = ilm + bytes(IVB_SIZE - len(ilm))
    replace_build_time = build_time is not None
    if build_time is None and template is None:
        build_time = utc_build_time()
    bt = (build_time.encode("ascii")[:16].ljust(16, b"_")
          if build_time is not None else None)
    if template is not None:
        if len(template) < HDR_SIZE:
            raise ValueError("MT7601 header template is shorter than 32 bytes")
        hdr = bytearray(template[:HDR_SIZE])
        struct.pack_into("<II", hdr, 0, len(ilm), len(dlm))
        if replace_build_time:
            hdr[16:32] = bt
        hdr = bytes(hdr)
    else:
        hdr = struct.pack("<IIHH", len(ilm), len(dlm), *version)
        hdr += b"\x00" * 4 + bt
        assert len(hdr) == HDR_SIZE
    return hdr + ilm + dlm


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ilm", required=True, type=Path)
    ap.add_argument("--dlm", type=Path)
    ap.add_argument("-o", "--output", required=True, type=Path)
    args = ap.parse_args()
    img = build(args.ilm.read_bytes(), args.dlm.read_bytes() if args.dlm else b"")
    args.output.write_bytes(img)
    print(f"wrote {args.output} {len(img)} bytes")


if __name__ == "__main__":
    main()
