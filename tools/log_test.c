#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t storage[0x1010 / 4];
#define MT7601U_LOG_RING_BASE ((uintptr_t)storage)

#include "../src/evidence.c"
#include "../src/log.c"

int main(void)
{
	uint8_t *ring = (uint8_t *)(storage + 4);
	mt7601u_log_ring_reset();
	mt7601u_log_event(MT_EVENT_BSP_READY, 0, 0);
	mt7601u_log_event(MT_EVENT_USB_SUSPEND, 2, 0);
	mt7601u_log_event(MT_EVENT_USB_RESUME, 1, 0);
	mt7601u_log_event(MT_EVENT_UNKNOWN_COMMAND, 31, 8);
	mt7601u_log_event(MT_EVENT_FIRMWARE_READY, 1, 1);
	assert(storage[2] == 0 && storage[3] == 4096);
	assert(fwrite(ring, 1, storage[1], stdout) == storage[1]);

	mt7601u_log_ring_reset();
#if INDEPENDENT_LOGGING
	assert(storage[0] == MT7601U_LOG_RING_MAGIC);
	for (int index = 0; index < 4096; index++)
		mt7601u_log_printf("x");
	mt7601u_log_printf("%u\n", 7u);
	assert(storage[1] == 4099 && storage[2] == 3);
	assert(memcmp(ring, "7\r\n", 3) == 0 && ring[4095] == 'x');
#else
	assert(storage[0] == MT7601U_EVIDENCE_MAGIC);
	for (int index = 0; index < 171; index++)
		mt7601u_log_event(MT_EVENT_BSP_READY, 0, 0);
	assert(storage[1] == 4104 && storage[2] == 8);
	assert(ring[0] == 0 && ring[4095] == 0);
#endif
	return 0;
}
