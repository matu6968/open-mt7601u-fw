#include "mt7601u_rf_bbp.h"
#include "mt7601u_mmio.h"
#include "mt7601u_log_ring.h"
#include "mt7601u_runtime.h"
static uint8_t report_done;
static void report(unsigned reg, int rf)
{
	uint8_t value = 0;
	enum mt7601u_rf_bbp_status status = rf ? mt7601u_rf_read(0, reg, &value) :
		mt7601u_bbp_read(reg, &value);
#if !INDEPENDENT_LOGGING
	MT7601U_REPORT(MT_RF_READ, 0, rf, reg, status, value);
#else
	mt7601u_log_printf("RFBBP read %s 0:%u status=%u value=%08x\n",
		rf ? "RF" : "BBP", reg, (unsigned)status, (unsigned)value);
#endif
}
void mt7601u_rf_bbp_boot_report(void)
{
	if (report_done)
		return;
	/* BootROM holds CSR/BBP reset until Linux MAC setup. Do not mistake
	 * successful CSR transactions returning zero for a ready baseband. */
	if (WLAN_REG(0x1004) & 3u) {
		mt7601u_log_printf("RFBBP: CSR/BBP reset held; waiting for host setup\n");
		return;
	}
	report_done = 1;
	report(12, 1);
	report(0, 0);
	report(3, 0);
}
