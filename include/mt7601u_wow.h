#ifndef MT7601U_WOW_H
#define MT7601U_WOW_H

#include <stdint.h>

uint32_t mt7601u_wow_command(uint32_t command, const uint8_t *payload, uint16_t length);
uint32_t mt7601u_wow_status(void);
uint32_t mt7601u_wow_rx_count(void);
void mt7601u_wow_usb_arm(int enabled);
void mt7601u_wow_suspend(void);
void mt7601u_wow_reset(void);
int mt7601u_wow_receive(const uint8_t *frame, unsigned length, uint32_t rxinfo);

#endif
