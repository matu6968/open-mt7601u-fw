#ifndef MT7601U_USB_DESC_H
#define MT7601U_USB_DESC_H

#include <stdint.h>

/* Byte i of a configuration descriptor as sent to the host. With
 * full_speed_bulk set, every bulk endpoint's wMaxPacketSize reads as 0x0040.
 *
 * This mirrors BootROM 0x100c6c. GET_DESCRIPTOR(config) passes 0 and
 * GET_DESCRIPTOR(other speed config) passes 1; when that argument equals the
 * speed flag at SRAM 0xFCDA (1 = high speed, see mt7601u_usb_link_high_speed)
 * the ROM rewrites the bulk endpoints of its descriptor copy at 0xFC00 to 64
 * bytes, sends it, and restores 512. Its interrupt and isochronous endpoints
 * are left alone, so only bulk endpoints are patched here.
 *
 * length bounds the parse; a truncated descriptor is handled because bytes
 * at or past length are never requested. */
static inline uint8_t mt7601u_usb_config_byte(const uint8_t *config,
		uint32_t length, uint32_t i, int full_speed_bulk)
{
	uint32_t offset = 0u;

	while (full_speed_bulk != 0 && offset + 1u < length && config[offset] != 0u) {
		uint32_t next = offset + config[offset];

		if (i < next) {
			if (config[offset + 1u] == 5u && config[offset] >= 7u &&
			    offset + 3u < length && (config[offset + 3u] & 3u) == 2u) {
				if (i == offset + 4u)
					return 0x40u;
				if (i == offset + 5u)
					return 0x00u;
			}
			break;
		}
		offset = next;
	}
	return config[i];
}

#endif
