#ifndef MT7601U_RF_BBP_H
#define MT7601U_RF_BBP_H
#include <stdint.h>

#define MT7601U_RF_PORT 0x00410500u
#define MT7601U_BBP_PORT 0x0041101cu
#define MT7601U_RF_BUSY 0x80000000u
#define MT7601U_RF_WRITE 0x40000000u
#define MT7601U_RF_FIELDS 0xc003ffffu
#define MT7601U_BBP_BUSY 0x00020000u
#define MT7601U_BBP_READ 0x00010000u
#define MT7601U_BBP_FIELDS 0x0003ffffu
#define MT7601U_RF_BBP_LIMIT 1000u

enum mt7601u_rf_bbp_status {
	MT7601U_RF_BBP_OK = 0,
	MT7601U_RF_BBP_INVALID = 1,
	MT7601U_RF_BBP_BUSY_TIMEOUT = 2,
	MT7601U_RF_BBP_COMPLETION_TIMEOUT = 3,
	MT7601U_RF_BBP_MISMATCH = 4,
	MT7601U_RF_BBP_POLL_TIMEOUT = 5,
};

/* Numeric MMIO values, not byte-cast spill slots. RF bank 0..15, register
 * 0..63; BBP register 0..255. Wider arguments allow invalid inputs to be
 * rejected rather than truncated. Failed reads/updates leave output intact.
 * Transactions and masked updates preserve GIE and exclude EP0 IRQ access.
 * Linux's reg_atomic_mutex serializes host direct accesses, but does not
 * exclude MCU calibration commands. Firmware must preserve a pending host
 * halfword pair and the completed host CSR result across a calibration.
 * These functions do not enable WLAN clocks or release CSR/BBP reset. */
enum mt7601u_rf_bbp_status mt7601u_rf_read(unsigned bank, unsigned reg, uint8_t *value);
enum mt7601u_rf_bbp_status mt7601u_rf_write(unsigned bank, unsigned reg, uint8_t value);
enum mt7601u_rf_bbp_status mt7601u_bbp_read(unsigned reg, uint8_t *value);
enum mt7601u_rf_bbp_status mt7601u_bbp_write(unsigned reg, uint8_t value);
/* Strict masked assignment: (old & ~mask) | (value & mask). No write if
 * unchanged. No readback-value verification: trigger bits may self-clear. */
enum mt7601u_rf_bbp_status mt7601u_rf_update(unsigned bank, unsigned reg,
	uint8_t mask, uint8_t value, uint8_t *updated);
enum mt7601u_rf_bbp_status mt7601u_bbp_update(unsigned reg,
	uint8_t mask, uint8_t value, uint8_t *updated);
/* Caller owns/aligned-address-validates the MMIO port for poll32. All bounds
 * are 1..LIMIT; expected must be a subset of mask. last is optional and gets
 * the last successful sample (including a polling timeout). */
enum mt7601u_rf_bbp_status mt7601u_poll32(uint32_t address, uint32_t mask,
	uint32_t expected, unsigned limit, uint32_t *last);
enum mt7601u_rf_bbp_status mt7601u_rf_poll(unsigned bank, unsigned reg,
	uint8_t mask, uint8_t expected, unsigned limit, uint8_t *last);
enum mt7601u_rf_bbp_status mt7601u_bbp_poll(unsigned reg,
	uint8_t mask, uint8_t expected, unsigned limit, uint8_t *last);
/* One read-only report after CSR/BBP reset release; boot or first Linux CAL. */
void mt7601u_rf_bbp_boot_report(void);
struct mt7601u_rf_bbp_ports {
	uint32_t rf;
	uint32_t bbp;
};
/* Caller excludes EP0 service across save, calibration and restore. Save
 * waits for real hardware completion; restore never writes a busy port and
 * never replays a kick. RF/BBP register changes made by calibration remain. */
enum mt7601u_rf_bbp_status mt7601u_rf_bbp_save_ports(struct mt7601u_rf_bbp_ports *ports);
enum mt7601u_rf_bbp_status mt7601u_rf_bbp_restore_ports(const struct mt7601u_rf_bbp_ports *ports);
#endif
