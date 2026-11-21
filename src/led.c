#include "mt7601u_runtime.h"

/* The two words per output drive the LED pattern generator. Its four
 * enable/source fields share the bytes of SYS+0x270. */
#define LED_MUX		0x270u
#define LED_ACTIVITY_01	0x274u
#define LED_ACTIVITY_23	0x278u
#define LED_PATTERN	0x27cu
#define LED_REPEAT	0x280u
#define LED_SW_SOURCE	0x82u
#define LED_MAC_SOURCE	0x84u

int mt7601u_led_mode_valid(uint32_t channel, uint32_t mode)
{
	return channel < 4u && (mode <= 6u || mode == 0x10u);
}

void mt7601u_led_set_mode(uint32_t channel, uint32_t mode)
{
	static const uint32_t patterns[] = {
		0x00010000u, /* Steady on. */
		0x00000000u, /* Off. */
		0x00000000u, /* MAC activity uses the shared timing words. */
		0x32320001u,
		0x19190001u,
		0x11110001u,
		0x0a320001u,
	};
	uint32_t source = LED_SW_SOURCE;
	uint32_t shift = channel * 8u;

	if (mode == 2u) {
		SYS_REG(LED_ACTIVITY_01) = 0x03070307u;
		SYS_REG(LED_ACTIVITY_23) = 0x03070307u;
		source = LED_MAC_SOURCE;
	} else {
		SYS_REG(LED_PATTERN + shift) = mode == 0x10u ?
			0x11110190u : patterns[mode];
		SYS_REG(LED_REPEAT + shift) = mode == 0x10u ? 0x01000001u : 0u;
	}

	/* Keep other outputs' source, enable, and polarity intact. */
	SYS_REG(LED_MUX) = (SYS_REG(LED_MUX) & ~(0xffu << shift)) |
		(source << shift);
}
