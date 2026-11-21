#include "mt7601u_cmd.h"
#include "mt7601u_runtime.h"
#include "mt7601u_wow.h"

#define FCE_ROUTE_MASK		(3u << 24)
#define FCE_ROUTE_MCU		(1u << 24)
#define USB_WAKE_ENABLE		(1u << 17)
#define SYS_USB_WAKE_SIGNAL	(1u << 7)
#define RX_ERRORS		(7u << 8)
#define RX_AMSDU		(1u << 11)
#define RX_PADDING		(1u << 14)
#define RX_DECRYPTED		(1u << 16)

static struct {
	uint8_t station[6];
	uint8_t bssid[6];
	uint8_t enabled, magic, associated, usb_method;
	uint8_t usb_armed, suspended, routed, signalled, woke;
	uint32_t saved_route;
	uint32_t rx_count;
} wow;

static uint32_t word(const uint8_t *value)
{
	return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
		(uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static void stop_routing(void)
{
	wow.suspended = 0;
	if (wow.routed) {
		WLAN_REG(0x80c) = (WLAN_REG(0x80c) & ~FCE_ROUTE_MASK) | wow.saved_route;
		wow.routed = 0;
	}
	if (wow.signalled) {
		SYS_REG(0x80) &= ~SYS_USB_WAKE_SIGNAL;
		wow.signalled = 0;
	}
}

uint32_t mt7601u_wow_command(uint32_t command, const uint8_t *payload, uint16_t length)
{
	if (command == CMD_WOW_CONFIG) {
		if (!wow.enabled || wow.routed || length < 4u)
			return 1;
		switch (word(payload)) {
		case 1: /* Magic detection mask, no auxiliary events. */
			if (length != 16u || word(payload + 4) != 1u ||
			    word(payload + 8) != 1u || word(payload + 12) != 0u)
				return 1;
			wow.magic = 1;
			return 0;
		case 3: /* Station address, BSSID, connected. */
			if (length != 20u || word(payload + 16) != 1u ||
			    (payload[4] & 1u) || (payload[10] & 1u))
				return 1;
			for (unsigned i = 0; i < 6; i++) {
				wow.station[i] = payload[4 + i];
				wow.bssid[i] = payload[10 + i];
			}
			wow.associated = 1;
			return 0;
		default:
			return 1;
		}
	}

	if (length != 8u || word(payload + 4) > 1u)
		return 1;
	uint32_t option = word(payload + 4);
	switch (word(payload)) {
	case 1: /* Enable. */
		stop_routing();
		wow.enabled = (uint8_t)option;
		wow.magic = wow.associated = wow.usb_method = 0;
		wow.woke = 0;
		if (option)
			wow.rx_count = 0;
		return 0;
	case 3: /* Switch RX between host and MCU. */
		if (!option) {
			stop_routing();
			return 0;
		}
		if (!wow.enabled || !wow.magic || !wow.associated || !wow.usb_method)
			return 1;
		if (!wow.routed) {
			uint32_t route = WLAN_REG(0x80c);

			wow.saved_route = route & FCE_ROUTE_MASK;
			WLAN_REG(0x80c) = (route & ~FCE_ROUTE_MASK) | FCE_ROUTE_MCU;
			wow.routed = 1;
		}
		return 0;
	case 4: /* USB in-band wake. */
		if (!wow.enabled || wow.routed || option != 1u)
			return 1;
		wow.usb_method = 1;
		return 0;
	default:
		return 1;
	}
}

uint32_t mt7601u_wow_status(void)
{
	return 0x574f5700u | ((uint32_t)wow.routed << 1) | wow.woke;
}

uint32_t mt7601u_wow_rx_count(void)
{
	return wow.rx_count;
}

void mt7601u_wow_usb_arm(int enabled)
{
	/* The controller may clear the hardware enable bit on USB suspend. */
	wow.usb_armed = enabled != 0;
	if (!enabled)
		stop_routing();
}

void mt7601u_wow_suspend(void)
{
	wow.suspended = wow.usb_armed && wow.routed;
}

void mt7601u_wow_reset(void)
{
	mt7601u_wow_usb_arm(0);
	wow.enabled = wow.magic = wow.associated = wow.usb_method = 0;
}

static int equal_address(const uint8_t *first, const uint8_t *second)
{
	for (unsigned i = 0; i < 6; i++)
		if (first[i] != second[i])
			return 0;
	return 1;
}

static int magic_payload(const uint8_t *payload, unsigned length)
{
	static const uint8_t sync[6] = { 255, 255, 255, 255, 255, 255 };

	for (; length >= 102u; length--, payload++) {
		unsigned i;

		if (!equal_address(payload, sync))
			continue;
		for (i = 1; i <= 16; i++)
			if (!equal_address(payload + i * 6u, wow.station))
				break;
		if (i == 17)
			return 1;
	}
	return 0;
}

static int is_magic_frame(const uint8_t *frame, unsigned length, uint32_t rxinfo)
{
	unsigned header = 24u;

	/* Infrastructure data from the associated AP. Check the MAC verdict
	 * before examining any protected payload. */
	if (length < header || (rxinfo & (RX_ERRORS | RX_AMSDU)) ||
	    (frame[0] & 0x7fu) != 0x08u || (frame[1] & 7u) != 2u ||
	    (frame[22] & 15u) ||
	    ((frame[1] & 0x40u) && !(rxinfo & RX_DECRYPTED)) ||
	    !equal_address(frame + 10, wow.bssid) ||
	    (!(frame[4] & 1u) && !equal_address(frame + 4, wow.station)))
		return 0;
	if (frame[0] & 0x80u)
		header += (frame[1] & 0x80u) ? 6u : 2u;
	if (length < header + 8u)
		return 0;
	/* MPDU length excludes the optional L2 alignment pad. */
	const uint8_t *payload = frame + header + ((rxinfo & RX_PADDING) ? 2u : 0u);
	return magic_payload(payload, length - header);
}

int mt7601u_wow_receive(const uint8_t *frame, unsigned length, uint32_t rxinfo)
{
	if (!wow.routed)
		return 0;
	if (wow.suspended)
		wow.rx_count++;
	if (wow.suspended && !wow.signalled && is_magic_frame(frame, length, rxinfo)) {
		/* USB wake sequence from IMILAB's in-band wake at ILM 0x3568. */
		USB_DMA_CFG |= USB_WAKE_ENABLE;
		SYS_REG(0x80) |= SYS_USB_WAKE_SIGNAL;
		wow.signalled = 1;
		wow.woke = 1;
	}
	return 1;
}
