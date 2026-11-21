#include "mt7601u_runtime.h"

/* The mainline driver's runtime test is exactly COM_REG0 == 1. COM_REG1 is
 * used as an upload DMA completion bit only while BootROM services FCE; once
 * this image runs it publishes the image identity there instead. */
int mt7601u_mailbox_publish_running(struct mt7601u_runtime *runtime)
{
	const uint32_t transport = MT7601U_STAGE_USB | MT7601U_STAGE_FCE |
		MT7601U_STAGE_DMA;

	/* The first served EP0 read confirms the host can poll the mailbox. */
	if ((runtime->stage_mask & (transport | MT7601U_STAGE_ALIVE)) != transport ||
	    !runtime->usb_ep0_reads)
		return 0;

	WLAN_COM_REG1 = 0x494E4450u; /* "INDP" */
	WLAN_COM_REG0 = 1u;
	runtime->stage_mask |= MT7601U_STAGE_ALIVE;
	mt7601u_log_event(MT_EVENT_FIRMWARE_READY, 1u, runtime->usb_ep0_reads);
	return 1;
}
