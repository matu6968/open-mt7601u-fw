#include "mt7601u_runtime.h"

void mt7601u_boot_init(struct mt7601u_runtime *runtime)
{
	/* The stock reset path masks SYS sources before BSP setup.  Keep this
	 * independent of the Andes CSR INT_MASK used by entry.S. */
	SYS_REG(0x220u) |= 0x10u;
	SYS_REG(0x508u) = 0u;
	SYS_REG(0x50Cu) = 0u;
	SYS_REG(0x534u) = 0u;
	SYS_REG(0x528u) = 0x15u;
	SYS_REG(0x514u) = 3u;
	SYS_REG(0x510u) = 0xF7u;
	SYS_REG(0x20u) |= 0x10000u;
	SYS_REG(0x418u) = 8u;
	SYS_REG(0x428u) = 8u;
	SYS_REG(0x410u) = 0x4Eu;
	SYS_REG(0x418u) = 0x98u;
	SYS_REG(0x20u) |= 0x800u;
	SYS_REG(0x20u) &= ~0x20000u;
	runtime->stage_mask |= MT7601U_STAGE_BOOT;
}
