#ifndef MT7601U_BW_DPD_H
#define MT7601U_BW_DPD_H
#include <stdint.h>
#define MT7601U_DPD_SEARCH_LIMIT 128u
struct mt7601u_bw_dpd_result {
 uint32_t attempts, status, cleanup_status, measurements, polls, timer_last;
 int32_t temperature, metric[2];
 uint8_t valid, coefficients[4], gain[2], amplitude[2], disabled[2];
};
struct mt7601u_dpd_state { int32_t temperature; uint8_t valid, disabled[2]; };
extern struct mt7601u_dpd_state mt7601u_dpd_state;
extern struct mt7601u_bw_dpd_result mt7601u_bw_result, mt7601u_dpd_result, mt7601u_tssi_result;
uint32_t mt7601u_bw_calibrate(uint32_t value, unsigned shared_filter);
uint32_t mt7601u_dpd_calibrate(uint32_t temperature);
uint32_t mt7601u_tssi_kick(uint32_t value);
int32_t mt7601u_bw_word_sample(uint32_t word);
int32_t mt7601u_dpd_metric(const uint8_t samples[11], uint8_t tail);
uint8_t mt7601u_dpd_scale(uint8_t value, unsigned index);
uint32_t mt7601u_dpd_setting(int32_t temperature, unsigned mode);
uint8_t mt7601u_dpd_mode(int32_t temperature, unsigned mode);
#endif
