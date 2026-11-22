# Open MT7601U firmware

RAM firmware for MediaTek MT7601U USB 802.11n adapters. The target is station
operation with Linux's in-tree `mt7601u` driver. Linux handles association, key
installation, channel selection, and rate control. The MAC handles hardware
cipher processing. The firmware handles device startup, USB and DMA, eFuse
reads, and radio calibration.

## Build

Install GNU Make, Python 3.10 or later, and a host C compiler. Bootstrapping the
cross compiler also requires a C++ compiler, GNU build tools, and the GMP, MPFR
and MPC development packages.

```sh
make toolchain
make -j
make -j verbose
make check
```

The bootstrap builds GCC 16 and binutils 2.47 for `nds32be-elf` from local
source trees: `toolchain/gcc` (GCC, 16.2.1 was used) and
`toolchain/binutils-gdb`, which may be symlinks. Set `GCC_SRC` and
`BINUTILS_SRC` to use other locations. Nothing is downloaded. To use an
existing installation, pass its installation directory as
`NDS32_PREFIX=/path/to/nds32be-elf`. The build uses
little-endian runtime data and the NDS32 N9 instruction set.

The Makefile defaults to `OPT=-Os` and `LTOFLAGS=-flto`. The quiet image is
32,808 bytes with LTO and 35,360 bytes without (`make LTOFLAGS=`), against
52,968 bytes for the previous layout. Only the vectors (`0x0`), entry (`0xa4`), IQ trace (`0xc000`),
packet diagnostics (`0xc900`) and log ring (`0xd000`) have fixed addresses;
the rest of the ILM image is packed contiguously and zero-initialised runtime
state is not uploaded.

| Image | Diagnostics |
| --- | --- |
| `target_firmware/mt7601u.bin` | Binary calibration and runtime event records |
| `target_firmware/mt7601u-verbose.bin` | Human-readable calibration and IMILAB-derived messages |

The build produces matching ELF and map files and checks placement, firmware
format, and stack bounds. `make check` also exercises the command dispatcher,
key-table diagnostic redaction, and both log formats. `make clean` removes
build outputs.

The 32-byte header records the ELF link time in UTC as `YYYYMMDDHHMM____`.
Set `SOURCE_DATE_EPOCH` to a Unix timestamp for a reproducible clean build.
The uploaded ILM ends with the USB string descriptors for `MediaTek`,
`802.11 n WLAN`, and `1.0`, followed by the `Vers` record. The header and
record carry the same open firmware build and version.

## Load and roll back on Linux

Use the filename requested by your kernel (`modinfo -F firmware mt7601u` and the
firmware request in the kernel log). Place the selected binary under the
corresponding relative path in `/lib/firmware/updates/`. Save any existing file
at that location before replacing it. Unplug the adapter and reconnect it to
load the new image from BootROM. Confirm firmware loading and PHY registration
in the kernel log.

To roll back, restore the saved override or remove the override you added,
then unplug and reconnect the adapter. The driver uploads the image to RAM.
The normal firmware path reads eFuse for board calibration data.

Linux confirms a cold load by polling mailbox register `0x0730` for `1`.
Its `Firmware running!` message is available through kernel dynamic debug.
Both images also record firmware readiness in the [diagnostic ring](LOGGING.md).

## Linux hotspot mode

The companion [Linux AP driver patch](linux/README.md) adds a single 2.4 GHz
hotspot interface using the existing firmware. NetworkManager recognizes the
adapter's AP capability with the patched module loaded.

## Wake on WLAN

The companion Linux driver can arm wake on a magic packet while the adapter
is associated as a station. The firmware checks incoming frames from the
associated AP and signals USB remote wake for a packet matching the station
MAC. Enable it with the [Linux module and `iw` settings](linux/README.md#wake-on-wlan).

## LED indicator

The firmware implements Andes LED command 16 for steady on, steady off,
MAC activity, and hardware blink patterns. The companion Linux driver
exposes manual brightness control when a board LED channel is selected.
See the [Linux LED setup](linux/README.md#led-indicator).

## Support and evidence

The station command interface implements queue selection, atomic TSSI, the
bandwidth-filter selector, burst and tagged writes, bounded log reads, and
calibration IDs 1, 4, 5, 6, 7, 8, and 9. An unsupported request fails when it
requests a response. Sequence zero suppresses the response.

See [qualification status](QUALIFICATION.md) for exact-image testing and
[firmware diagnostics](LOGGING.md) for event provenance. WEP40, WEP104, and
TKIP/MIC have Linux hardware-key paths and await end-to-end validation.

Firmware source is GPLv2 under [COPYING](COPYING). See
[NOTICES.txt](NOTICES.txt) for source provenance. Report issues at the
[project repository](https://github.com/matu6968/open-mt7601u-fw).
