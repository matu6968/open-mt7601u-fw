#ifndef MT7601U_CALIBRATION_H
#define MT7601U_CALIBRATION_H
#include <stdint.h>
/* 0..5 retain shared serial-access errors. Commands remain CMD31 IDs 1/9. */
enum mt7601u_cal_status {
	MT7601U_CAL_NOT_READY = 6,
	MT7601U_CAL_ACTIVE = 7,
	MT7601U_CAL_TIMER = 8,
	MT7601U_CAL_RESTORE = 9,
};
struct mt7601u_cal_result {
	uint32_t attempts, status, cleanup_status, timer_last;
	int32_t code;
	uint8_t first, second, programmed, last_trigger;
	uint8_t valid, dc[6];
};
extern struct mt7601u_cal_result mt7601u_cal_r, mt7601u_cal_txdcoc;
/* Pure reference signed arithmetic. Return the unwrapped value; RF takes its
 * low byte, including the original's asymmetric rounding for negative deltas. */
int32_t mt7601u_cal_r_code(uint8_t first, uint8_t second);
/* Called in the serialized foreground command dispatcher. Host owns PHY
 * tables, clocks, VCO and runtime order. No BootROM/vendor calls. */
uint32_t mt7601u_calibrate(unsigned id);
#endif
