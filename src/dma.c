#include "mt7601u_runtime.h"

void mt7601u_dma_init(struct mt7601u_runtime *runtime)
{
	mt7601u_packet_init(runtime);
	if (runtime->fault) return;
	runtime->stage_mask |= MT7601U_STAGE_DMA;
}
