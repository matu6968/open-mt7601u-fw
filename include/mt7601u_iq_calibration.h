#ifndef MT7601U_IQ_CALIBRATION_H
#define MT7601U_IQ_CALIBRATION_H
#include <stdint.h>
#define MT7601U_IQ_TRACE_BASE 0x0000c000u
#define MT7601U_IQ_TRACE_END 0x0000c804u
#define MT7601U_IQ_TRACE_LIMIT 256u
/* Trace is overwritten at each measured command; no USB command is added. */
struct mt7601u_iq_result {
	uint32_t attempts, status, cleanup_status, measurements, polls, power;
	uint32_t raw[5], rms_i, rms_q;
	int32_t mean_i, mean_q, gain, phase;
	uint8_t valid, measured, gain_index, coefficients[6];
};
extern struct mt7601u_iq_result mt7601u_iq_loft, mt7601u_iq_tx, mt7601u_iq_rx;
/* Existing CMD31 IDs 4/5/8. Bit 0 selects retained-result reapplication. */
uint32_t mt7601u_iq_calibrate(unsigned id, uint32_t value);
/* Exact reference arithmetic, explicit signed interpretation and wrapping.
 * The ten-step Newton approximation starts at 600; it is not floor(sqrt). */
uint32_t mt7601u_iq_sqrt(uint32_t value);
uint32_t mt7601u_iq_power(uint32_t sample);
void mt7601u_iq_decode_rx(struct mt7601u_iq_result *result);
#endif
