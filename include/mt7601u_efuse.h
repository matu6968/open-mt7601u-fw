#ifndef MT7601U_EFUSE_H
#define MT7601U_EFUSE_H

#include <stdint.h>

#define MT7601U_EFUSE_CTRL 0x00410024u
#define MT7601U_EFUSE_DATA 0x00410028u
#define MT7601U_EFUSE_AOUT 0x0000003fu
#define MT7601U_EFUSE_MODE 0x000000c0u
#define MT7601U_EFUSE_AIN  0x03ff0000u
#define MT7601U_EFUSE_KICK 0x40000000u
#define MT7601U_EEPROM_SIZE 256u
#define MT7601U_EFUSE_MAP_START 0x1e0u
#define MT7601U_EFUSE_MAP_SIZE 29u
#define MT7601U_EFUSE_BLOCKS 18u
#ifndef MT7601U_EFUSE_POLL_LIMIT
#define MT7601U_EFUSE_POLL_LIMIT 1000u
#endif

enum mt7601u_efuse_mode {
	MT7601U_EFUSE_LOGICAL = 0,
	MT7601U_EFUSE_PHYSICAL = 1,
};

enum mt7601u_efuse_status {
	MT7601U_EFUSE_OK = 0,
	MT7601U_EFUSE_UNMAPPED = 1,
	MT7601U_EFUSE_INVALID = 2,
	MT7601U_EFUSE_TIMEOUT = 3,
	MT7601U_EFUSE_NOT_READ = 4,
};

enum mt7601u_efuse_valid_field {
	MT7601U_EE_VALID_CHIP = 1u << 0,
	MT7601U_EE_VALID_VERSION = 1u << 1,
	MT7601U_EE_VALID_MAC = 1u << 2,
	MT7601U_EE_VALID_NIC = 1u << 3,
	MT7601U_EE_VALID_COUNTRY = 1u << 4,
	MT7601U_EE_VALID_FREQ = 1u << 5,
	MT7601U_EE_VALID_LNA = 1u << 6,
	MT7601U_EE_VALID_RSSI = 1u << 7,
	MT7601U_EE_VALID_TEMP = 1u << 8,
	MT7601U_EE_VALID_TSSI = 1u << 9,
	MT7601U_EE_VALID_BW40 = 1u << 10,
};

/* Diagnostic interpretations, not effective RF settings or invented defaults.
 * logical[] remains authoritative, including erased and out-of-range bytes. */
struct mt7601u_efuse_config {
	uint32_t valid_fields;
	uint32_t rate_valid;
	uint16_t channel_valid;
	uint16_t chip_id;
	uint16_t nic[3];
	int16_t frequency_offset;
	uint8_t version_fae, version_ee, mac[6], country;
	int8_t lna_gain, rssi_offset[2], reference_temp;
	int8_t channel_power[14], rate_power[20], bw40_delta;
	uint8_t tssi_enabled, tssi_slope, tssi_offset[3];
	uint8_t tssi_base_offset, tssi_target_power;
	/* One validity bit for each NIC word's low/high byte, in word order. */
	uint8_t nic_byte_valid, rx_paths, tx_paths, board_type;
	uint8_t hw_rf_control, temperature_alc, external_lna_2g, external_lna_5g, tx_alc;
	uint8_t rx_streams, tx_streams, antenna_diversity, xtal_option;
	uint8_t temperature_disabled, coex_method;
};

struct mt7601u_efuse_snapshot {
	uint8_t logical[MT7601U_EEPROM_SIZE];
	uint8_t usage_map[MT7601U_EFUSE_MAP_SIZE];
	uint8_t block_status[MT7601U_EFUSE_BLOCKS];
	uint8_t complete;
	uint8_t status;
	uint16_t error_address;
	struct mt7601u_efuse_config config;
};

/* Addresses must be 16-byte aligned. Logical range is 0..0xf0; physical
 * access is intentionally restricted to the two usage-map blocks. On invalid
 * input/timeout, output is untouched. UNMAPPED fills the block with 0xff.
 * Caller must exclusively own the controller for the entire transaction. */
enum mt7601u_efuse_status mt7601u_efuse_read_block(
	enum mt7601u_efuse_mode mode, uint16_t address, uint8_t output[16]);
enum mt7601u_efuse_status mt7601u_efuse_snapshot_init(
	struct mt7601u_efuse_snapshot *snapshot);
void mt7601u_efuse_decode(struct mt7601u_efuse_snapshot *snapshot);
const struct mt7601u_efuse_config *mt7601u_efuse_config_get(
	const struct mt7601u_efuse_snapshot *snapshot);
void mt7601u_efuse_log(const struct mt7601u_efuse_snapshot *snapshot);

#endif
