#ifndef MT7601U_LOG_RING_H
#define MT7601U_LOG_RING_H

#include <stdint.h>

/* Both station images use the same ILM ring. */
#ifndef MT7601U_LOG_RING_BASE
#define MT7601U_LOG_RING_BASE       0x0000d000u
#endif
#define MT7601U_LOG_RING_MAGIC      0x4d544c47u /* "MTLG" */
#define MT7601U_LOG_RING_CAPACITY   4096u
#define MT7601U_LOG_RING_DATA       (MT7601U_LOG_RING_BASE + 0x10u)
#define MT7601U_LOG_RING_END        (MT7601U_LOG_RING_DATA + MT7601U_LOG_RING_CAPACITY)
#define MT7601U_LOG_RING_MASK       (MT7601U_LOG_RING_CAPACITY - 1u)

/* Four 32-bit words at BASE: magic, total characters, overwritten
 * characters, capacity. CMD_RANDOM_READ returns these values as little-endian
 * words on the inband wire. Character data begins at BASE+0x10 and wraps. */
#define MT7601U_LOG_RING_WORDS      4u

/* Read-only custom-firmware IRQ diagnostics, separate from the log ring and
 * below the stack. Words: magic, foreground PSW, actual vector entries,
 * interrupted IPSW, SYS pending, three reserved words,
 * SYS enabled, USB events, two reserved words, IPC, reserved, entry PSW,
 * vector target. Host acceptance requires task-level GIE and continuing IRQs. */
#define MT7601U_IRQ_DIAG_BASE       0x0000E020u
#define MT7601U_IRQ_DIAG_END        (MT7601U_IRQ_DIAG_BASE + 64u)
#define MT7601U_IRQ_DIAG_MAGIC      0x4d544952u /* "MTIR" */

#endif
