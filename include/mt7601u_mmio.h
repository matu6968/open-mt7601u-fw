#ifndef MT7601U_MMIO_H
#define MT7601U_MMIO_H

#include <stdint.h>

/* MCU-view addresses. Host vendor wIndex is the 16-bit offset in the
 * WLAN window (COM_REG0 host 0x0730 = MCU 0x00410730). Do not store to
 * the host-numbered address from Andes code — that locks USB.
 */

#define MT_MCU_ILM_BASE		0x00000000u
#define MT_MCU_IROM_BASE	0x00100000u
#define MT_MCU_DLM_BASE		0x00080000u
#define MT_MCU_SYS_BASE		0x00400000u
#define MT_MCU_USB_BASE		0x00401000u
#define MT_MCU_WLAN_BASE	0x00410000u
#define MT_MCU_BBP_BASE		0x40000000u
#define MT_MCU_RF_BASE		0x80000000u

#ifndef MMIO32
#define MMIO32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#endif
#ifndef MMIO16
#define MMIO16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#endif
#ifndef MMIO8
#define MMIO8(addr)  (*(volatile uint8_t *)(uintptr_t)(addr))
#endif

#define SYS_REG(off)  MMIO32(MT_MCU_SYS_BASE + (off))
#define USB_REG(off)  MMIO32(MT_MCU_USB_BASE + (off))
#define WLAN_REG(off) MMIO32(MT_MCU_WLAN_BASE + (off))

/* SYS / Andes INTC (MCU-only) */
#define SYS_BOOT_STRAP		SYS_REG(0x200)
#define SYS_INT_MASK		SYS_REG(0x204)
#define SYS_INT_STATUS		SYS_REG(0x210)
#define SYS_INT_ENABLE		SYS_REG(0x214)
#define SYS_INT_ROUTE		SYS_REG(0x23C)
#define SYS_REG_64		SYS_REG(0x064)
/* start_kernel writes this last, immediately before the FCE scheduler is
 * released.  Kept for parity with the vendor boot sequence. */
#define SYS_RUN			SYS_REG(0x230)

/* Stock rtmp_printf's character sink.  SYS+0x51c bit 5 means that the
 * debug character register at SYS+0x504 can accept a byte. */
#define SYS_DEBUG_TX		SYS_REG(0x504)
#define SYS_DEBUG_STATUS	SYS_REG(0x51C)
#define SYS_DEBUG_TX_READY	0x20u

/* Host MAC offsets inside the WLAN overlay */
#define MT_HOST_FCE_DMA_ADDR	0x0230u
#define MT_HOST_FCE_DMA_LEN	0x0234u
#define MT_HOST_USB_DMA_CFG	0x0238u
#define MT_HOST_WPDMA_GLO_CFG	0x0208u
#define MT_HOST_PBF_SYS_CTRL	0x0400u
#define MT_HOST_PBF_CFG		0x0404u
#define MT_HOST_COM_REG0	0x0730u
#define MT_HOST_COM_REG1	0x0734u
#define MT_HOST_COM_REG2	0x0738u
#define MT_HOST_COM_REG3	0x073Cu
#define MT_HOST_FCE_PSE_CTRL	0x0800u
#define MT_HOST_FCE_CPU_CMD_BASE	0x0980u
#define MT_HOST_FCE_CPU_CMD_IDX	0x0988u
#define MT_HOST_FCE_CPU_CMD_CPU_IDX	0x098Cu
#define MT_HOST_FCE_CMD_BASE	0x0990u
#define MT_HOST_FCE_CMD_IDX	0x0998u
#define MT_HOST_FCE_CMD_CPU_IDX	0x099Cu
#define MT_HOST_FCE_TX0_BASE	0x09A0u
#define MT_HOST_FCE_TX0_IDX	0x09A8u
#define MT_HOST_FCE_TX0_CPU_IDX	0x09ACu
#define MT_HOST_FCE_TX1_BASE	0x09B0u
#define MT_HOST_FCE_TX1_IDX	0x09B8u
#define MT_HOST_FCE_PDMA_GLOBAL_CONF	0x09C4u
#define MT_HOST_MAC_CSR0	0x1000u
#define MT_HOST_MAC_SYS_CTRL	0x1004u
#define MT_HOST_BBP_CSR_CFG	0x101Cu
#define MT_HOST_MAC_STATUS	0x1200u

#define FCE_DMA_ADDR		WLAN_REG(MT_HOST_FCE_DMA_ADDR)
#define FCE_DMA_LEN		WLAN_REG(MT_HOST_FCE_DMA_LEN)
#define USB_DMA_CFG		WLAN_REG(MT_HOST_USB_DMA_CFG)
#define PBF_SYS_CTRL		WLAN_REG(MT_HOST_PBF_SYS_CTRL)
#define WLAN_COM_REG0		WLAN_REG(MT_HOST_COM_REG0)
#define WLAN_COM_REG1		WLAN_REG(MT_HOST_COM_REG1)
#define WLAN_COM_REG2		WLAN_REG(MT_HOST_COM_REG2)
#define WLAN_COM_REG3		WLAN_REG(MT_HOST_COM_REG3)
#define FCE_PSE_CTRL		WLAN_REG(MT_HOST_FCE_PSE_CTRL)
#define FCE_CPU_CMD_BASE	WLAN_REG(MT_HOST_FCE_CPU_CMD_BASE)
#define FCE_CPU_CMD_IDX8	MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_CPU_CMD_IDX)
#define FCE_CPU_CMD_CPU_IDX8	MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_CPU_CMD_CPU_IDX)
#define FCE_CMD_BASE		WLAN_REG(MT_HOST_FCE_CMD_BASE)
#define FCE_CMD_CPU_IDX		WLAN_REG(MT_HOST_FCE_CMD_IDX)
#define FCE_CMD_CPU_IDX8	MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_CMD_IDX)
#define FCE_CMD_CPU_IDX_HOST8	MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_CMD_CPU_IDX)
#define FCE_TX0_BASE		WLAN_REG(MT_HOST_FCE_TX0_BASE)
#define FCE_TX0_IDX8		MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_TX0_IDX)
#define FCE_TX0_CPU_IDX8	MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_TX0_CPU_IDX)
#define FCE_TX1_BASE		WLAN_REG(MT_HOST_FCE_TX1_BASE)
#define FCE_TX1_IDX8		MMIO8(MT_MCU_WLAN_BASE + MT_HOST_FCE_TX1_IDX)
#define FCE_TX1_CPU_IDX8		MMIO8(MT_MCU_WLAN_BASE + 0x09BCu)
#define FCE_PDMA_GLOBAL_CONF	WLAN_REG(MT_HOST_FCE_PDMA_GLOBAL_CONF)
#define MAC_CSR0		WLAN_REG(MT_HOST_MAC_CSR0)
#define MAC_SYS_CTRL		WLAN_REG(MT_HOST_MAC_SYS_CTRL)
#define MAC_STATUS		WLAN_REG(MT_HOST_MAC_STATUS)

/* Endpoint bank selectors (IROM bootrom_usb_init writes 0x8B to these). */
#define USB_EP_BANK		USB_REG(0x098)
#define USB_CTRL_168C		USB_REG(0x68C)
#define USB_EVENT_STATUS	USB_REG(0x628)
/* FUN_4b0c/FUN_4b18/FUN_4b24 use 32-bit accesses (swi333/swi450), as do
 * BootROM's copies. The default 16-bit accessors are retained for the USB
 * width A/B build; saved captures confirm a COM_REG0 read with both widths,
 * but do not establish equivalence for every EP0 request or event path.
 * INDEPENDENT_USB_WORD_REGS selects the stock-identical width. */
#define USB_RX_COUNT		MMIO32(MT_MCU_USB_BASE + 0x000u)
/* Stock FUN_4b0c/4b18/4b24/4b44 are word accesses (swi333/lwi333). */
#define USB_RX_COUNT_WRITE	MMIO32(MT_MCU_USB_BASE + 0x000u)
#define USB_TX_COUNT		MMIO32(MT_MCU_USB_BASE + 0x004u)
#define USB_FIFO_CONTROL	MMIO32(MT_MCU_USB_BASE + 0x008u)
/* Stock FUN_5c58 calls FUN_4c04 with USB+0x400 for EP0 TX mode 0. FUN_5ba0
 * reads host-to-device bytes from the spaced RX lanes at USB+0x500. The
 * descriptor map uses other FIFO banks; do not substitute USB+0x410 here. */
#define USB_EP0_FIFO_BASE	(MT_MCU_USB_BASE + 0x400u)
#define USB_EP0_FIFO		USB_EP0_FIFO_BASE
#define USB_TX_FIFO		USB_EP0_FIFO_BASE
#define USB_RX_FIFO		(MT_MCU_USB_BASE + 0x500u)
#define USB_SETUP_BASE		(MT_MCU_USB_BASE + 0x600u)
#define USB_FIFO_STATUS		USB_RX_COUNT

/* USB+0x630 is written to acknowledge events (FUN_5650 writes 1 after a
 * SETUP, FUN_5758 writes 0x20, FUN_59dc/FUN_56ec and the IROM write 0x3f) and
 * is also read by BootROM 0x10132C and stock FUN_597C to dispatch bits
 * 0/3/4/5.  Polling it from the foreground re-enters the endpoint programmer
 * if bit 5 is acted on, so the custom runtime still uses bit 0 only. */
#define USB_CMD_630		USB_REG(0x630)
/* start_kernel writes 2 here while bringing up the command path. */
#define USB_REG_710		USB_REG(0x710)

/* FUN_3a90 is the only writer of the WLAN event pair in the vendor ILM:
 * handler table at 0xf748, then WLAN 0x9e0 and 0x9e8 each OR the slot bit.
 * WLAN 0x1e0/0x1e8 have no xref anywhere in the vendor image. */
#define WLAN_EVENT_STATUS	WLAN_REG(0x9E0)
#define WLAN_EVENT_ENABLE	WLAN_REG(0x9E8)

#define HELO			0x48454C4Fu

/* PBF_SYS_CTRL bit 0 is MCU_RESET — never set on USB. */

#endif
