# Linux MT7601U access point support

The patch advertises one AP interface through mac80211, alongside the
existing station interface mode. It uses the existing station firmware's USB
register and burst-write interfaces. The driver uploads a bounded,
zero-padded beacon template to WLAN beacon RAM and arms the MAC TSF and TBTT
timer. Beacon changes and unicast TIM updates refresh the template. The
hardware repeats the template with DTIM period 1. Concurrent AP/STA, CAC,
and advanced AP power-save behavior are outside this milestone.

The MAC beacon programming approach is based on the published Linux
[MT7601U AP proposal](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-August/1162095.html).
This patch concentrates on one 2.4 GHz AP and uses the existing key and
station management paths.

## Build and select the module

Pass a local Linux Git checkout with the `v7.0` tag and install headers for
the kernel that will load the module. The builder extracts a clean revision
of the MT7601U source, applies the AP, WoWLAN, and LED patches, and builds an
external module. It leaves in-progress edits in the Linux checkout alone.

```sh
./linux/build-ap-module.sh /path/to/linux
```

Use `LINUX_REF` to select a different compatible revision and `KDIR` to
select a different headers directory. Linux `v7.0` and the local revision
`cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a` both built against Ubuntu
`7.0.0-43-generic` headers. The latter provided the module for the hardware
test.

Install the resulting `build/linux-ap/mt7601u.ko` as a kernel-specific
module override under `/lib/modules/$(uname -r)/updates/mt7601u-ap/`, run
`depmod -a`, then reload `mt7601u`. The in-tree module stays on disk for
rollback. Install the selected firmware image under its firmware lookup
name if your host needs one. The module may need signing on systems that
enforce module signature verification.

The hotspot can be started in GNOME Settings with the MT7601U selected, or
with NetworkManager's `nmcli device wifi hotspot ifname INTERFACE`.
`nmcli -g WIFI-PROPERTIES.AP device show INTERFACE` should report `yes`.
To roll back, remove the override module, run `depmod -a`, and reload the
packaged driver. Remove firmware overrides you added if you want to restore
the previous firmware lookup.

## Hardware result

An MT7601U (`148f:7601`) running the verbose independent firmware advertised
AP mode to NetworkManager. NetworkManager started a 2.4 GHz WPA-Personal
hotspot on channel 6. A second Wi-Fi radio saw the SSID. A Galaxy S21
associated, received a DHCP address, fetched an HTTP page served by the host,
and returned five ICMP replies with zero loss. The existing host uplink
stayed on the separate Intel Wi-Fi interface.

The local override was then tested through a physical USB power cycle. The
kernel uploaded the verbose firmware, loaded the AP module from
`updates/mt7601u-ap/`, and NetworkManager still reported AP capability.
The temporary validation hotspot was removed.

## Wake on WLAN

Load the companion module with `wowlan=1` and use a firmware image with
WoWLAN support. The module checks the firmware before advertising the
magic-packet trigger. With the adapter connected to an AP as a station,
enable the trigger on its PHY:

```sh
iw dev
sudo iw phy phyN wowlan enable magic-packet
iw phy phyN wowlan show
```

Send the magic packet from another device on the same LAN. `iw event -t`
shows a `magic packet received` wake reason on resume. The USB port needs
to keep the dongle powered and permit remote wake during suspend. Use
`sudo iw phy phyN wowlan disable` to turn off the trigger.

The host uses the [stock Andes command layouts](https://github.com/OpenIPC/mt7601u/blob/0ac46553f3190d788b01c15cbeaa14f2951c55a3/common/cmm_asic.c#L3080-L3282)
for the magic detection mask, the station and AP addresses, USB wake
selection, and RX routing. A
device vendor IN request `0x2f` with zero value and index returns a
little-endian status word. The base value is `0x574f5700`. Bit 0 reports
a magic-packet wake and bit 1 reports that RX is routed to the firmware.
An eight-byte read includes a second word counting frames received by the
firmware while the port was suspended. See the [hardware results](../QUALIFICATION.md).

## LED indicator

Load the companion module with `led_channel=1` for the channel used by
[TP-Link’s TL-WN727N v4 driver](https://www.tp-link.com/uk/support/download/tl-wn727n/v4/).
Other boards can select channels 0 through 3 according to their wiring.
The default `led_channel=-1` leaves the LED class control disabled.
The module exposes `brightness` and `max_brightness` under
`/sys/class/leds/<USB-interface>::wlan/`. Write `1` to turn on the LED or
`0` to turn it off.

For firmware-level control, Andes command 16 takes one or two pairs of
little-endian 32-bit words. Each pair contains the channel and control
value. Values 0, 1, and 2 select steady on, off, and MAC activity respectively.
Values 3 through 6 and `0x10` select stock hardware blink patterns.
