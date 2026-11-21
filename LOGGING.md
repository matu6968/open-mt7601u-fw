# Firmware diagnostics

The 4 KiB log ring starts at ILM `0xd000`. Its four little-endian header
words give the magic, total bytes written, total overwritten bytes, and
capacity. Data starts at `0xd010`. Read it with the existing EP0
`0x20` / `0x23` memory-read interface. A live collector should read deltas
between coherent header snapshots and detect gaps when the ring wraps.

The quiet image uses magic `0x4d544447` and version-1 binary records.
`python3 tools/decode_evidence.py capture.bin` decodes an uninterrupted
capture from cursor zero. The verbose image uses `0x4d544c47` and writes its
text directly into the SRAM ring.

Both images record `Firmware ready: COM_REG0=1, USB reads=1` when they publish
the Linux readiness mailbox. This follows USB, FCE, and DMA initialization
and the first served EP0 register read. The quiet image encodes it as
runtime event 5.

The reference is IMILAB ILM SHA-256
`37423fb94e00d2df59ae43f5fe2e99261ab851bf4965595e223e9cecbb3f267e`.
Addresses are ILM addresses verified against the NDS32 disassembly.

| Message | Stock string | Stock print call | Production event |
| --- | --- | --- | --- |
| BSP Init done. | `0xc764` | `0x3a84` | BSP register setup completed |
| ResumeIRQ | `0xc7d0` | `0x58ee` | Foreground observed USB reset/resume event count advancing |
| Suspend! | `0xc7dc` | `0x5966` | Foreground observed USB suspend event count advancing |
| Unknown command | `0xc3d0` | `0x2502` | Unsupported MCU command ID and length, up to eight per boot |
| reset FCE | `0xc81c` | `0x5f98` | Stock reset-to-ROM path only. The native firmware's ring initialization and USB reset handler do not perform that FCE reset. |

The USB mailbox preserves operation, stage, and address metadata. Values from
the pairwise and shared key tables, IV/EIV, and associated attributes are
masked in snapshots. USB SETUP value and raw trap registers are outside the
logged metadata.
