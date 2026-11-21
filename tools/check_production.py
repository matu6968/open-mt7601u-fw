"""Verify production image bytes and run the real command and logging code on a host."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

from decode_evidence import decode
from pack_mt76_elf import region_image, ILM_BASE, ILM_LIMIT


def usb_string(value):
    encoded = value.encode('utf-16le')
    return bytes((2 + len(encoded), 3)) + encoded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    expected = ('BSP Init done.\nSuspend! count=2\nResumeIRQ count=1\n'
                'Unknown command:31, len:8\nFirmware ready: COM_REG0=1, USB reads=1\n')
    descriptors = b'\x04\x03\x09\x04' + b''.join(
        usb_string(value) for value in ('MediaTek', '802.11 n WLAN', '1.0'))
    descriptors = descriptors.ljust((len(descriptors) + 3) & ~3, b'\0')
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / 'firmware-test'
        flags = [*shlex.split(args.cc), '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-ffunction-sections', '-Wl,--gc-sections', '-Iinclude']
        subprocess.run([*flags, '-DINDEPENDENT_LOGGING=0', 'tools/firmware_test.c',
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        for verbose, name in enumerate(('mt7601u', 'mt7601u-verbose')):
            subprocess.run([*flags, f'-DINDEPENDENT_LOGGING={verbose}', 'tools/log_test.c',
                            '-o', str(binary)], check=True)
            log = subprocess.check_output([str(binary)])
            assert (log.decode().replace('\r\n', '\n') if verbose else decode(log)) == expected
            data = (args.output / f'{name}.bin').read_bytes()
            size, dlm, build, version = struct.unpack_from('<IIHH', data)
            assert dlm == 0 and size == len(data) - 32
            elf = args.output / f'{name}.elf'
            epoch = int(os.environ.get('SOURCE_DATE_EPOCH',
                                       elf.stat().st_mtime_ns // 1_000_000_000))
            stamp = datetime.fromtimestamp(epoch, timezone.utc).strftime('%Y%m%d%H%M____')
            assert data[16:32] == stamp.encode('ascii')
            assert data.endswith(descriptors + struct.pack('<4sIHH', b'Vers', 0xb1, build, version))
            assert data[32:] == region_image(elf.read_bytes(), ILM_BASE, ILM_LIMIT)
            for marker in (b'BSP Init done.', b'Suspend!', b'ResumeIRQ',
                           b'Unknown command:', b'Firmware ready:'):
                assert (marker in data) == bool(verbose)
    print('Production image, command, redaction, and log checks passed')


if __name__ == '__main__':
    main()
