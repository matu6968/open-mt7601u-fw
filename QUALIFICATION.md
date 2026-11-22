# Qualification status

The standalone source and offline checks are available. Both candidate
images carried live 2.4 GHz WPA2-PSK/CCMP traffic on the same phone hotspot.
The full hardware release gate remains pending.

The hardware series uses unmodified IMILAB as a control against the same
phone and LAN traffic peer. Each candidate needs three clean cold starts
with scan, association, DHCP, and 35 seconds each of bidirectional TCP and
UDP. Each candidate also needs supplicant, interface, USB, and hotspot
recovery, CCMP metadata, and 15 minutes of passing traffic. Record observed
group decryption and in-place renewals. Open networks, scheduled rekey, and
legacy ciphers require separate evidence for a support claim.

Two clean builds of the original station qualification images passed
`make check` and reproduced both images byte for byte using GCC 9.5.0
and binutils 2.35.2. The hashes below identify the images used for the
traffic results.

These hashes and the results below predate the compact ILM layout and the
GCC 16.2.1 / binutils 2.47 toolchain (optionally with `-flto`). The new layout
and toolchain passed `make check`, `check_efuse_layout.py` and
`check_mt7601u_stack.py`, and two clean builds were byte-identical, but only a
single scan, association and kernel-log comparison has been run on hardware,
with the verbose image built with GCC 16.2.1 and `-flto` (35,824 bytes, build
time 202311142213). It loaded, ran, read the EEPROM and associated, and the
upload stage was about 3.5 ms faster than the previous image. They are not
qualified: the series above has not been repeated, and the quiet LTO image and
the non-LTO builds have not been run on hardware.

| Image | SHA-256 |
| --- | --- |
| Quiet | `f151450eec3ce751d381ec3d333aa9e510844e4bc038855e2eb9d56ba4944e08` |
| Verbose | `e1fc97a63a96392d1a3d17bb645e6770e5ad99f8bacaa3a8c841b4ca7670cf63` |

## Live Linux results, 2026-11-21

The USB adapter was `148f:7601`, silicon revision `76010001`, on Linux
`7.0.0-43-generic`. The packaged in-tree driver reported srcversion
`55B6E51E1EB4C388D831423`.

| Image | TCP upload / download | UDP upload / download | Station evidence |
| --- | --- | --- | --- |
| Quiet | 4.43 / 4.89 Mbps | 4.99 / 4.98 Mbps | Passed both traffic flows, supplicant reconnect, and interface cycling. Rejoined after a hotspot restart with a channel change from 6 to 11 and passed 20 recovery pings. The subsequent TCP upload measured 3.70 Mbps, below the 3.75 Mbps gate. |
| Verbose | 4.53 / 4.90 Mbps | 4.75 / 4.97 Mbps | Passed both traffic flows, supplicant reconnect, and interface cycling. USB reset re-enumerated the adapter, and it rejoined and passed 20 recovery pings. The next iperf3 flow timed out. |

Both passing runs correlated the installed pairwise key slot with TX and
hardware-decrypted unicast RX, and both recorded hardware-decrypted group
frames. Neither run observed an in-place PTK or GTK renewal. IMILAB passed
control runs on the same hotspot and also failed some control runs.
The station series stopped before completing three accepted cold starts,
the 15-minute soaks, open-mode testing, and suspend/resume testing.

The project maintainer also reported a successful short WPA3 run. This is a
smoke result. The firmware hash, negotiated AKM, and traffic measurements
were not captured in the qualification logs.

## Build metadata and cold boot, 2026-11-21

With the AP-capable Linux module, both images below loaded from BootROM
after a USB power cycle. Linux printed the generated header time
`202611212012____` and `Firmware running!` with that call site enabled via
dynamic debug. Both returned the expected USB strings, exposed
`COM_REG0=1` and `COM_REG1=0x494e4450`, and recorded the firmware-ready
event on the first serviced register read. NetworkManager reported AP
capability and returned scan results for each image.

| Image | SHA-256 |
| --- | --- |
| Quiet | `7591de8ed6a0f186585402b58d0ed3f1b095c9746de89cfa8c74fdccd1b670c9` |
| Verbose | `5399509f46894d3d8e8b8e2b6b0304096bc2788fe9416ba777af9299b9876f94` |

## Wake on WLAN, 2026-11-21

The directly connected `148f:7601` ran the verbose firmware below with the
companion module built from Linux `v7.0` against Ubuntu
`7.0.0-43-generic` headers. The adapter was associated with a 2.4 GHz AP.
The test enabled wake on the upstream USB hub and controller and used
suspend-to-idle with a 70-second RTC fallback. A separate LAN host sent a
wrong-MAC broadcast first, then magic packets for the adapter.

| Run | Time asleep | RX frames handled in suspend | Linux wake reason | Post-resume ping |
| --- | --- | --- | --- | --- |
| 1 | 22.65 s | 784 | Magic packet | 5/5 |
| 2 | 22.81 s | 796 | Magic packet | 5/5 |

Both runs stayed asleep through the wrong-MAC packet, woke before the RTC
alarm, and retained the USB device number. An earlier run through the USB
power relay reached the RTC fallback and re-enumerated the adapter.

| Artifact | SHA-256 |
| --- | --- |
| Verbose firmware | `26712b75cfd77b88db647a1e7167e02ca6f771b1cd69d2f4c02b202b66f352c4` |
| Companion module | `6a0f082b0d320ec14ee8c59d29a20fb5e6a96c36c3fa7543f1365dc11f610340` |

## LED command, 2026-11-21

The IMILAB firmware implements Andes command 16 at ILM address `0x16b4`.
The TP-Link Linux driver for the TL-WN727N v4 sends the command to channel
1. The production command test checks all supported hardware modes,
dual-channel requests, and validation before register writes.

On the current MT7601U, two Linux LED class on/off cycles changed the
channel-1 pattern at SYS+`0x284` between `0x00010000` and `0x00000000`.
The other outputs’ register values matched their pre-test values. The
adapter passed five station pings, and NetworkManager reported AP
capability. This adapter has no visible LED. Optical verification on a
TL-WN727N v4 remains pending.

| Artifact | SHA-256 |
| --- | --- |
| Verbose firmware | `5900e39bedeb98f69fd60c253e1bee398c7d88874c2af0126469790a3e04b311` |
| Companion module | `75e9c29591acd474b6acc360b8342b1a412be5c181a5a4e04b948b791a51fe1b` |

## EEPROM utility compatibility, 2026-11-22

The unmodified [`mt7601u_eeprom`](https://github.com/xurubin/mt7601u_eeprom/)
userspace tool at `09bd372` worked with the companion Linux
`7.0.0-43-generic` module (srcversion `446C00669DF6FF7E34269E5`) and the
verbose 35,824-byte firmware (build time `202611221114____`, installed-image
SHA-256 `f97823b4f40b74b660b03f2ba6d34823c7cb143555502c84884f50f704082ba6`).

Both `dump` modes read 32 blocks. The 256-byte `save` matched the mapped
physical blocks and all 16 logical blocks in the firmware's separate boot
snapshot. `flash_test` proposed zero block changes for the saved image and
six for the utility's bundled sample. The 512-byte physical dump matched
before and after both dry runs. This run exercised readout and planning.
The utility's bundled kernel module failed to build against the current
kernel headers with mac80211 callback signature mismatches.
