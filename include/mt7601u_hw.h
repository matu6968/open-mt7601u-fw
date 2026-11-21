#ifndef MT7601U_HW_H
#define MT7601U_HW_H

#include <stdint.h>

#include "mt7601u_mmio.h"
#include "mt7601u_packet.h"

/* Host offsets mirrored in the MCU WLAN view. These are the small common
 * subset needed by the first MAC/DMA bring-up stages; new registers should be
 * added only with a Linux/original-ILM reference. */
#define MT7601U_INT_SOURCE_CSR 0x0200u
#define MT7601U_INT_MASK_CSR 0x0204u
#define MT7601U_WPDMA_GLO_CFG 0x0208u
#define MT7601U_WPDMA_RST_IDX 0x020Cu
#define MT7601U_TX_RING_BASE 0x0300u
#define MT7601U_RX_RING_BASE 0x03C0u
#define MT7601U_RING_STRIDE 0x10u
#define MT7601U_PBF_CFG 0x0404u
#define MT7601U_RF_CSR_CFG 0x0500u
#define MT7601U_MAC_ADDR_DW0 0x1008u
#define MT7601U_MAC_ADDR_DW1 0x100Cu
#define MT7601U_MAC_BSSID_DW0 0x1010u
#define MT7601U_MAC_BSSID_DW1 0x1014u
#define MT7601U_MAX_LEN_CFG 0x1018u
#define MT7601U_BBP_CSR_CFG 0x101Cu
#define MT7601U_BEACON_TIME_CFG 0x1114u
#define MT7601U_INT_TIMER_CFG 0x1128u
#define MT7601U_INT_TIMER_EN 0x112Cu
#define MT7601U_RX_FILTR_CFG 0x1400u

#define MT7601U_INT_RX_DONE_ALL ((1u << 0) | (1u << 1))
#define MT7601U_INT_TX_DONE_ALL (0x3FFu << 4)
#define MT7601U_INT_MCU_CMD (1u << 19)
#define MT7601U_INT_TBTT (1u << 20)
#define MT7601U_INT_PRE_TBTT (1u << 21)
#define MT7601U_INT_GPTIMER (1u << 24)

#define MT7601U_MAC_SYS_RESET_CSR (1u << 0)
#define MT7601U_MAC_SYS_RESET_BBP (1u << 1)
#define MT7601U_MAC_SYS_ENABLE_TX (1u << 2)
#define MT7601U_MAC_SYS_ENABLE_RX (1u << 3)

/* The FCE descriptors are byte-addressed little-endian records. TX0 is the
 * host-to-MCU command ring; its two buffers are 200 bytes each, matching the
 * stock FUN_43fc initialization. */
#define MT7601U_FCE_RING_ENTRIES 2u
#define MT7601U_FCE_DESC_BYTES 16u
#define MT7601U_FCE_CMD_BYTES 1024u
#define MT7601U_FCE_TX0_BYTES 200u
#define MT7601U_FCE_TX1_BYTES 0x640u

struct mt7601u_fce_ring {
	uint8_t desc[MT7601U_FCE_RING_ENTRIES][MT7601U_FCE_DESC_BYTES];
	void *info[MT7601U_FCE_RING_ENTRIES];
};

struct mt7601u_fce_state {
	struct mt7601u_fce_ring tx0;
	struct mt7601u_fce_ring tx1;
	struct mt7601u_fce_ring cpu_cmd;
	struct mt7601u_fce_ring cmd_resp;
	uint8_t tx0_payload[MT7601U_FCE_RING_ENTRIES][MT7601U_FCE_CMD_BYTES];
	uint8_t tx1_payload[MT7601U_FCE_RING_ENTRIES][MT7601U_FCE_TX1_BYTES];
	uint8_t response_payload[MT7601U_FCE_RING_ENTRIES][MT7601U_FCE_CMD_BYTES];
	uint8_t idx_tx0;
	uint8_t idx_tx1;
	uint8_t idx_cmd;
	uint8_t idx_resp;
	uint8_t reserved[4];
	uint32_t count_a4;
	uint32_t count_a8;
	uint32_t count_commands;
	uint32_t count_responses;
	uint32_t count_bad;
	uint32_t count_dropped;
	uint32_t last_txinfo;
	uint8_t qsel;
	uint8_t bw_filter_mode;
	uint8_t unknown_reports;
};

#define MT7601U_FCE_CPU_CMD_BASE 0x0980u
#define MT7601U_FCE_CPU_CMD_IDX 0x0988u
#define MT7601U_FCE_CPU_CMD_CPU_IDX 0x098Cu
#define MT7601U_FCE_CMD_BASE 0x0990u
#define MT7601U_FCE_CMD_CPU_IDX 0x099Cu
#define MT7601U_FCE_TX0_BASE 0x09A0u
#define MT7601U_FCE_TX0_CPU_IDX 0x09ACu
#define MT7601U_FCE_TX1_BASE 0x09B0u
#define MT7601U_FCE_BASE_REG(off) WLAN_REG(off)
#define MT7601U_FCE_CPU_CMD_MAX MT7601U_FCE_BASE_REG(0x0984u)
#define MT7601U_FCE_CMD_MAX MT7601U_FCE_BASE_REG(0x0994u)
#define MT7601U_FCE_TX0_MAX MT7601U_FCE_BASE_REG(0x09A4u)
#define MT7601U_FCE_TX1_MAX MT7601U_FCE_BASE_REG(0x09B4u)
#define MT7601U_FCE_PDMA_BUSY_MASK ((1u << 1) | (1u << 3))

#define MT7601U_USB_DMA_RX_BULK_EN (1u << 22)
#define MT7601U_USB_DMA_TX_BULK_EN (1u << 23)
#define MT7601U_USB_EVENT_READY 1u

#define MT7601U_TXINFO_LEN_MASK  0x0000FFFFu
#define MT7601U_TXINFO_SEQ_MASK  0x000F0000u
#define MT7601U_TXINFO_CMD_MASK  0x07F00000u
#define MT7601U_TXINFO_DPORT_MASK 0x38000000u
#define MT7601U_TXINFO_TYPE_MASK 0xC0000000u
#define MT7601U_TXINFO_SEQ_SHIFT 16
#define MT7601U_TXINFO_CMD_SHIFT 20
#define MT7601U_TXINFO_DPORT_SHIFT 27
#define MT7601U_TXINFO_TYPE_SHIFT 30

#define MT7601U_CPU_TX_PORT 2u
#define MT7601U_CPU_RX_PORT 1u
#define MT7601U_DMA_PACKET 0u
#define MT7601U_DMA_COMMAND 1u

static inline uint32_t mt7601u_txinfo_command(uint16_t length,
		uint8_t seq, uint8_t command)
{
	return (uint32_t)length |
		((uint32_t)(seq & 0xfu) << MT7601U_TXINFO_SEQ_SHIFT) |
		((uint32_t)(command & 0x7fu) << MT7601U_TXINFO_CMD_SHIFT) |
		(MT7601U_CPU_TX_PORT << MT7601U_TXINFO_DPORT_SHIFT) |
		(MT7601U_DMA_COMMAND << MT7601U_TXINFO_TYPE_SHIFT);
}

static inline uint8_t mt7601u_txinfo_seq(uint32_t info)
{
	return (uint8_t)((info & MT7601U_TXINFO_SEQ_MASK) >>
			MT7601U_TXINFO_SEQ_SHIFT);
}

static inline uint8_t mt7601u_txinfo_command_id(uint32_t info)
{
	return (uint8_t)((info & MT7601U_TXINFO_CMD_MASK) >>
			MT7601U_TXINFO_CMD_SHIFT);
}

#endif
