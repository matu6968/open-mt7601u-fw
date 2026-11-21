#ifndef MT7601U_USB_H
#define MT7601U_USB_H

#include "mt7601u_cmd.h"

#define MT7601U_USB_OBJ		((void *)0x0000F8D8u)
#define MT7601U_POOL_TX0	((void *)0x0000F8C8u)
#define MT7601U_POOL_TX1	((void *)0x0000F8B8u)

/* Adapter at 0xF8D8. FUN_3bdc / usb_fce_init_c. CMD_RESP descs at +0x60. */
struct mt7601u_usb {
	uint8_t tx0_desc[2][16];
	uint8_t tx1_desc[2][16];
	uint8_t cmd_desc[2][16];
	uint8_t rsp_desc[2][16];
	void *tx0_info[2];
	void *tx1_info[2];
	void *cmd_info[2];
	void *rsp_info[2];
	uint8_t idx_tx0;
	uint8_t idx_tx1;
	uint8_t idx_cmd;
	uint8_t idx_rsp;
	uint32_t cnt_a4;
	uint32_t cnt_a8;
	int32_t hw;
	int32_t sw;
	uint32_t irq_bits;
	void *list_next;
	void *list_prev;
	uint32_t pad_c0;
};

int cmd_send_response(void *usb, struct mt7601u_cmd_info *info);
void cmd_fill_response(struct mt7601u_cmd_info *info, uint32_t status);
void usb_fce_init(void *usb);
void usb_fce_init_c(uint8_t *usb);

#endif
