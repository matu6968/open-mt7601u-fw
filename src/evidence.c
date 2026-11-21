#include "mt7601u_runtime.h"
#include "mt7601u_log_ring.h"
#if !INDEPENDENT_LOGGING
static uint32_t sequence, epoch;
void mt7601u_evidence_reset(void)
{
	sequence = epoch = 0;
}
void mt7601u_evidence_emit(unsigned type, const uint32_t *words, unsigned count)
{
	volatile uint32_t *header = (volatile uint32_t *)(uintptr_t)MT7601U_LOG_RING_BASE;
	volatile uint8_t *data = (volatile uint8_t *)(uintptr_t)MT7601U_LOG_RING_DATA;
	uint32_t fields[3], total = header[1];
	unsigned i, j;
	/* Records have a 12-byte header: version, type, word count, sequence,
	 * epoch, followed by explicitly little-endian words. Single writer. */
	if (type == MT_CAL && count && words[0] == 1)
		epoch++;
	fields[0] = 1u | (type << 8) | (count << 16);
	fields[1] = sequence++;
	fields[2] = epoch;
	for (i = 0; i < count + 3; i++) {
		uint32_t word = i < 3 ? fields[i] : words[i - 3];
		for (j = 0; j < 4; j++)
			data[total++ & MT7601U_LOG_RING_MASK] = word >> (j * 8);
	}
	__asm__ volatile ("" ::: "memory");
#ifdef __NDS32__
	__builtin_nds32_dsb();
#endif
	header[2] = total > MT7601U_LOG_RING_CAPACITY ? total - MT7601U_LOG_RING_CAPACITY : 0;
	header[1] = total;
}
#endif
