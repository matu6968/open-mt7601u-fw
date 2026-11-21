#include "mt7601u_calibration.h"
#include "mt7601u_rf_bbp.h"
#include "mt7601u_mmio.h"
#include "mt7601u_runtime.h"
#ifndef CAL_READ32
#define CAL_READ32(a) MMIO32(a)
#define CAL_WRITE32(a,v) (MMIO32(a) = (v))
#endif
#define MAC 0x00411004u
#define BYPASS 0x00410504u
#define SETTING 0x0041050cu
#define TIMER_STATUS 0x00400400u
#define TIMER_CTRL 0x00400418u
#define TIMER_LIMIT 100000u
struct mt7601u_cal_result mt7601u_cal_r, mt7601u_cal_txdcoc;
#if INDEPENDENT_LOGGING
static const char rf_restore_log[] __attribute__((section(".caldiag"))) =
	"CAL restore RF %u:%u write=%u read=%u expected=%02x actual=%02x\n";
static const char bbp_restore_log[] __attribute__((section(".caldiag"))) =
	"CAL restore BBP %u write=%u read=%u expected=%02x actual=%02x\n";
#endif

int32_t mt7601u_cal_r_code(uint8_t first, uint8_t second)
{
	int32_t a = first < 128u ? first : (int32_t)first - 256;
	int32_t b = second < 128u ? second : (int32_t)second - 256;
	int32_t q = ((b - a) * 1000) / 43;
	if (q % 10 >= 5)
		q += 10;
	return q / 10;
}

static uint32_t ready(void)
{
	uint8_t version;
	uint32_t status;
	if ((CAL_READ32(0x00410080u) & 3u) != 3u || (CAL_READ32(MAC) & 3u))
		return MT7601U_CAL_NOT_READY;
	status = mt7601u_bbp_read(0, &version);
	if (status) return status;
	return version && version != 255u ? 0u : MT7601U_CAL_NOT_READY;
}

static void first_error(uint32_t *error, uint32_t status)
{ if (!*error && status) *error = status; }
static uint32_t rf_restore(unsigned bank, unsigned reg, uint8_t value)
{
	uint8_t readback;
	uint32_t status = mt7601u_rf_write(bank, reg, value);
	/* A failed posted write may nevertheless have reached hardware. Readback
	 * cannot erase its error, but also must not prevent the other restores. */
	uint32_t check = mt7601u_rf_read(bank, reg, &readback);
	/* The original RF 5:4 rewrite probe establishes 44 -> 04 after
	 * analog forcing (also used by IQ/BW cleanup). Warm Linux reprobes
	 * can snapshot 44 before R. Accept only that exact transition. */
	if (!check && bank == 5u && reg == 4u && value == 0x44u && readback == 0x04u)
		readback = value;
	if (status || check || readback != value)
		mt7601u_log_printf(rf_restore_log,
			bank, reg, status, check, (unsigned)value, check ? 0u : (unsigned)readback);
	if (status) return status;
	return check ? check : readback == value ? 0u : MT7601U_CAL_RESTORE;
}
static uint32_t bbp_restore(unsigned reg, uint8_t value)
{
	uint8_t readback;
	uint32_t status = mt7601u_bbp_write(reg, value);
	uint32_t check = mt7601u_bbp_read(reg, &readback);
	if (status || check || readback != value)
		mt7601u_log_printf(bbp_restore_log,
			reg, status, check, (unsigned)value, check ? 0u : (unsigned)readback);
	if (status) return status;
	return check ? check : readback == value ? 0u : MT7601U_CAL_RESTORE;
}
static void mmio_restore(uint32_t setting, uint32_t bypass, uint32_t *error)
{
	CAL_WRITE32(BYPASS, 0);
	CAL_WRITE32(SETTING, setting);
	CAL_WRITE32(BYPASS, bypass);
	if (CAL_READ32(SETTING) != setting || CAL_READ32(BYPASS) != bypass)
		first_error(error, MT7601U_CAL_RESTORE);
}

/* The existing 78-count system timer is 1ms in the reference (FUN_4a58).
 * It is unused by this firmware's IRQ dispatcher. Require exclusive ownership
 * and the established configuration, rearm once, and bound status reads.
 * Status bit 0 signals expiry; write 0x11 acknowledges timer 0.
 * ACK only timer-0 flags; no standby/tick callback or global IRQ masking. */
static uint32_t settle_tick(struct mt7601u_cal_result *result)
{
	uint32_t ctrl = CAL_READ32(TIMER_CTRL), n;
	if ((CAL_READ32(0x00400214u) & (1u << 12)) ||
	    CAL_READ32(0x00400410u) != 78u || (ctrl & ~0x80u) != 0x18u)
		return MT7601U_CAL_TIMER;
	CAL_WRITE32(TIMER_STATUS, 0x11u);
	CAL_WRITE32(TIMER_CTRL, ctrl | 0x80u);
	for (n = 0; n < TIMER_LIMIT; n++) {
		result->timer_last = CAL_READ32(TIMER_STATUS);
		if (result->timer_last & 1u) break;
	}
	CAL_WRITE32(TIMER_STATUS, 0x11u);
	CAL_WRITE32(TIMER_CTRL, ctrl);
	return n == TIMER_LIMIT ? MT7601U_CAL_TIMER : 0u;
}

static void short_settle(void)
{
	unsigned outer, inner;
	/* FUN_7450's second measurement delay: 60 groups of seven NOPs.
	 * Explicit iteration count, no invented microsecond clock contract. */
	for (outer = 0; outer < 60u; outer++)
		for (inner = 0; inner < 7u; inner++) __asm__ volatile ("nop");
}

static uint32_t txdcoc(struct mt7601u_cal_result *result)
{
	uint32_t setting, bypass, status;
	uint8_t saved;
	status = mt7601u_rf_read(5, 1, &saved);
	if (status) return status;
	if (saved & 4u) return MT7601U_CAL_ACTIVE;
	setting = CAL_READ32(SETTING); bypass = CAL_READ32(BYPASS);
	CAL_WRITE32(SETTING, 0xe010u); CAL_WRITE32(BYPASS, 0xffff8u);
	status = mt7601u_rf_write(5, 1, 4);
	if (!status)
		status = mt7601u_rf_poll(5, 1, 4, 0, MT7601U_RF_BBP_LIMIT, &result->last_trigger);
	if (!status) {
		unsigned i;
		for (i = 0; i < 6u; i++) {
			status = mt7601u_rf_read(5, 47u + i, &result->dc[i]);
			if (status) break;
		}
	}
	first_error(&result->cleanup_status, rf_restore(5, 1, saved));
	mmio_restore(setting, bypass, &result->cleanup_status);
	return status;
}

struct rf_saved { uint8_t bank, reg, temporary, before; };
static uint32_t r_cal(struct mt7601u_cal_result *result)
{
	struct rf_saved rf[] = {{5,4,0x27,0}, {5,17,0x80,0}, {5,18,0x83,0},
		{5,19,0,0}, {5,20,0x20,0}, {0,1,0,0}, {0,34,0x13,0}, {0,35,0,0}};
	uint32_t mac, setting, bypass, status, i;
	uint8_t saved47, saved7, value, readback;
	/* Snapshot everything before the first mutation. A failed snapshot has
	 * no analog side effects and needs no fabricated restoration values. */
	for (i = 0; i < sizeof(rf)/sizeof(rf[0]); i++) {
		status = mt7601u_rf_read(rf[i].bank, rf[i].reg, &rf[i].before);
		if (status) return status;
	}
	status = mt7601u_bbp_read(47, &saved47); if (status) return status;
	status = mt7601u_rf_read(0, 7, &saved7); if (status) return status;
	status = mt7601u_bbp_read(21, &value); if (status) return status;
	if (value) return MT7601U_CAL_ACTIVE;
	status = mt7601u_bbp_read(22, &value); if (status) return status;
	if (value) return MT7601U_CAL_ACTIVE;
	mac = CAL_READ32(MAC); setting = CAL_READ32(SETTING); bypass = CAL_READ32(BYPASS);
	CAL_WRITE32(MAC, 0); CAL_WRITE32(BYPASS, 0x1830u); CAL_WRITE32(SETTING, 0x2000u);
	for (i = 0; i < sizeof(rf)/sizeof(rf[0]); i++) {
		status = mt7601u_rf_write(rf[i].bank, rf[i].reg, rf[i].temporary);
		if (status) goto cleanup;
	}
#define STEP(operation) do { status = (operation); if (status) goto cleanup; } while (0)
	STEP(mt7601u_bbp_write(47, 4));
	STEP(mt7601u_bbp_write(22, 0x80));
	STEP(settle_tick(result));
	STEP(mt7601u_bbp_read(49, &result->first));
	STEP(mt7601u_bbp_write(22, 0));
	STEP(mt7601u_rf_write(0, 35, 1));
	STEP(mt7601u_bbp_write(22, 0x80));
	short_settle();
	STEP(mt7601u_bbp_read(49, &result->second));
	result->code = mt7601u_cal_r_code(result->first, result->second);
	result->programmed = (uint8_t)result->code;
	STEP(mt7601u_rf_write(0, 7, result->programmed));
	STEP(mt7601u_rf_read(0, 7, &readback));
	if (readback != result->programmed) status = MT7601U_RF_BBP_MISMATCH;
cleanup:
	/* Reference postconditions: stop BBP22 and pulse BBP21 then leave zero.
	 * Try every restore even if one port is stuck. Restore MAC last so traffic
	 * cannot resume while the RF bypass is still forced. */
	first_error(&result->cleanup_status, bbp_restore(22, 0));
	first_error(&result->cleanup_status, mt7601u_bbp_write(21, 1));
	first_error(&result->cleanup_status, bbp_restore(21, 0));
	for (i = 0; i < sizeof(rf)/sizeof(rf[0]); i++)
		first_error(&result->cleanup_status, rf_restore(rf[i].bank, rf[i].reg, rf[i].before));
	first_error(&result->cleanup_status, bbp_restore(47, saved47));
	mmio_restore(setting, bypass, &result->cleanup_status);
	if (status || result->cleanup_status)
		first_error(&result->cleanup_status, rf_restore(0, 7, saved7));
	CAL_WRITE32(MAC, mac);
	if (CAL_READ32(MAC) != mac) {
		first_error(&result->cleanup_status, MT7601U_CAL_RESTORE);
		CAL_WRITE32(MAC, 0);
		first_error(&result->cleanup_status, rf_restore(0, 7, saved7));
		CAL_WRITE32(MAC, mac);
	}
	return status;
#undef STEP
}

uint32_t mt7601u_calibrate(unsigned id)
{
	struct mt7601u_cal_result *result;
	uint32_t status;
	if (id != 1u && id != 9u) return MT7601U_RF_BBP_INVALID;
	result = id == 1u ? &mt7601u_cal_r : &mt7601u_cal_txdcoc;
	result->attempts++;
	result->valid = 0; result->cleanup_status = 0; result->timer_last = 0;
	result->first = result->second = result->programmed = result->last_trigger = 0;
	result->code = 0;
	{ unsigned i; for (i = 0; i < 6u; i++) result->dc[i] = 0; }
	status = ready();
	if (!status) status = id == 1u ? r_cal(result) : txdcoc(result);
	result->status = status ? status : result->cleanup_status ? MT7601U_CAL_RESTORE : 0u;
	result->valid = result->status == 0u;
	MT7601U_REPORT(MT_CAL, "CAL id=%u status=%u cleanup=%u first=%u second=%u code=%d byte=%u trigger=%u timer=%08x valid=%u dc=%02x%02x%02x%02x%02x%02x\n",
		id, result->status, result->cleanup_status, (unsigned)result->first,
		(unsigned)result->second, result->code, (unsigned)result->programmed,
		(unsigned)result->last_trigger, result->timer_last, (unsigned)result->valid,
		(unsigned)result->dc[0], (unsigned)result->dc[1], (unsigned)result->dc[2],
		(unsigned)result->dc[3], (unsigned)result->dc[4], (unsigned)result->dc[5]);
	return result->status;
}
