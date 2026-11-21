#ifndef MT7601U_CMD_H
#define MT7601U_CMD_H

#include <stdint.h>

/* Matches linux/drivers/net/wireless/mediatek/mt7601u/mcu.h and dma.h. */

enum mt7601u_mcu_cmd {
	CMD_FUN_SET_OP = 1,
	CMD_LOAD_CR = 2,
	CMD_INIT_GAIN_OP = 3,
	CMD_DYNC_VGA_OP = 6,
	CMD_TDLS_CH_SW = 7,
	CMD_BURST_WRITE = 8,
	CMD_READ_MODIFY_WRITE = 9,
	CMD_RANDOM_READ = 10,
	CMD_BURST_READ = 11,
	CMD_RANDOM_WRITE = 12,
	CMD_LED_MODE_OP = 16,
	CMD_POWER_SAVING_OP = 20,
	CMD_WOW_CONFIG = 21,
	CMD_WOW_QUERY = 22,
	CMD_WOW_FEATURE = 24,
	CMD_CARRIER_DETECT_OP = 28,
	CMD_RADOR_DETECT_OP = 29,
	CMD_SWITCH_CHANNEL_OP = 30,
	CMD_CALIBRATION_OP = 31,
	CMD_BEACON_OP = 32,
	CMD_ANTENNA_OP = 33,
};

enum mt7601u_mcu_function {
	Q_SELECT = 1,
	ATOMIC_TSSI_SETTING = 5,
};

enum mt7601u_mcu_calibrate {
	MCU_CAL_R = 1,
	MCU_CAL_DCOC,
	MCU_CAL_LC,
	MCU_CAL_LOFT,
	MCU_CAL_TXIQ,
	MCU_CAL_BW,
	MCU_CAL_DPD,
	MCU_CAL_RXIQ,
	MCU_CAL_TXDCOC,
};

enum mt7601u_msg_port {
	WLAN_PORT = 0,
	CPU_RX_PORT,
	CPU_TX_PORT,
	HOST_PORT,
};

enum mt7601u_info_type {
	DMA_PACKET = 0,
	DMA_COMMAND = 1,
};

enum mt7601u_evt_type {
	CMD_DONE = 0,
	CMD_ERROR,
	CMD_RETRY,
	EVENT_PWR_RSP,
	EVENT_WOW_RSP,
	EVENT_CARRIER_DETECT_RSP,
	EVENT_DFS_DETECT_RSP,
};

#define MT_TXD_INFO_LEN		0x0000FFFFu
#define MT_TXD_INFO_SEQ		0x000F0000u
#define MT_TXD_INFO_CMD		0x07F00000u
#define MT_TXD_INFO_D_PORT	0x38000000u
#define MT_TXD_INFO_TYPE	0xC0000000u

#define MT_CMD_TYPE_SHIFT	20
#define MT_CMD_SEQ_SHIFT	16

struct mt7601u_cmd_info {
	uint32_t pad[2];
	void *payload;
	uint16_t length;
	uint8_t flags;
	uint8_t rsp;
	uint32_t txinfo;
};

/* Imilab handler ABI: called by the stock dispatcher with jral5/ret5. */
uint32_t cmd_fun_set_op_c(void *payload, uint16_t length);

int cmd_dispatch(void *unused, struct mt7601u_cmd_info *info);

#endif
