#include "mt7601u_efuse.h"

/* Keep the proven control profile's executable and mutable state unchanged. */
#include "mt7601u_runtime.h"

#ifndef MT7601U_EFUSE_READ32
#define MT7601U_EFUSE_READ32(address) MMIO32(address)
#define MT7601U_EFUSE_WRITE32(address, value) (MMIO32(address) = (value))
#endif

static int wait_idle(void)
{
	uint32_t tries, pause;
	for (tries = 0; tries < MT7601U_EFUSE_POLL_LIMIT; tries++) {
		if ((MT7601U_EFUSE_READ32(MT7601U_EFUSE_CTRL) &
		     MT7601U_EFUSE_KICK) == 0u)
			return 1;
		/* An iteration bound, not an assumed CPU-clock/microsecond timer. */
		for (pause = 0; pause < 64u; pause++)
			__asm__ volatile ("nop");
	}
	return 0;
}

enum mt7601u_efuse_status mt7601u_efuse_read_block(
		enum mt7601u_efuse_mode mode, uint16_t address, uint8_t output[16])
{
	uint32_t control, value, i, j;
	if (output == (uint8_t *)0 || (address & 15u) != 0u ||
	    (mode != MT7601U_EFUSE_LOGICAL && mode != MT7601U_EFUSE_PHYSICAL) ||
	    (mode == MT7601U_EFUSE_LOGICAL && address >= MT7601U_EEPROM_SIZE) ||
	    (mode == MT7601U_EFUSE_PHYSICAL && address != 0x1e0u && address != 0x1f0u))
		return MT7601U_EFUSE_INVALID;
	if (!wait_idle())
		return MT7601U_EFUSE_TIMEOUT;
	control = MT7601U_EFUSE_READ32(MT7601U_EFUSE_CTRL);
	control &= ~(MT7601U_EFUSE_AIN | MT7601U_EFUSE_MODE);
	control |= ((uint32_t)address << 16) | ((uint32_t)mode << 6) |
		MT7601U_EFUSE_KICK;
	MT7601U_EFUSE_WRITE32(MT7601U_EFUSE_CTRL, control);
	if (!wait_idle())
		return MT7601U_EFUSE_TIMEOUT;
	control = MT7601U_EFUSE_READ32(MT7601U_EFUSE_CTRL);
	if ((control & MT7601U_EFUSE_AOUT) == MT7601U_EFUSE_AOUT) {
		for (i = 0; i < 16u; i++)
			output[i] = 0xffu;
		return MT7601U_EFUSE_UNMAPPED;
	}
	for (i = 0; i < 4u; i++) {
		value = MT7601U_EFUSE_READ32(MT7601U_EFUSE_DATA + i * 4u);
		for (j = 0; j < 4u; j++)
			output[i * 4u + j] = (uint8_t)(value >> (j * 8u));
	}
	return MT7601U_EFUSE_OK;
}

static uint16_t word(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

void mt7601u_efuse_decode(struct mt7601u_efuse_snapshot *snapshot)
{
	struct mt7601u_efuse_config *c = &snapshot->config;
	const uint8_t *e = snapshot->logical;
	uint32_t i, nonzero = 0u, not_ff = 0u;
	for (i = 0; i < sizeof(*c); i++)
		((uint8_t *)c)[i] = 0u;
	if (!snapshot->complete)
		return;
	c->chip_id = word(e);
	c->version_fae = e[2];
	c->version_ee = e[3];
	if (c->chip_id != 0xffffu)
		c->valid_fields |= MT7601U_EE_VALID_CHIP;
	if (e[2] != 0xffu && e[3] <= 0x0du)
		c->valid_fields |= MT7601U_EE_VALID_VERSION;
	for (i = 0; i < 6u; i++) {
		c->mac[i] = e[4u + i];
		nonzero |= c->mac[i];
		not_ff |= c->mac[i] ^ 0xffu;
	}
	if (nonzero && not_ff && (c->mac[0] & 1u) == 0u)
		c->valid_fields |= MT7601U_EE_VALID_MAC;
	c->nic[0] = word(e + 0x34);
	c->nic[1] = word(e + 0x36);
	c->nic[2] = word(e + 0x42);
	for (i = 0; i < 6u; i++)
		if (((c->nic[i / 2u] >> ((i & 1u) * 8u)) & 0xffu) != 0xffu)
			c->nic_byte_valid |= (uint8_t)(1u << i);
	if (c->nic_byte_valid == 0x3fu)
		c->valid_fields |= MT7601U_EE_VALID_NIC;
	c->rx_paths = c->nic[0] & 15u;
	c->tx_paths = (c->nic[0] >> 4) & 15u;
	c->board_type = (c->nic[0] >> 12) & 3u;
	c->hw_rf_control = c->nic[1] & 1u;
	c->temperature_alc = (c->nic[1] >> 1) & 1u;
	c->external_lna_2g = (c->nic[1] >> 2) & 1u;
	c->external_lna_5g = (c->nic[1] >> 3) & 1u;
	c->tx_alc = (c->nic[1] >> 13) & 1u;
	c->rx_streams = c->nic[2] & 15u;
	c->tx_streams = (c->nic[2] >> 4) & 15u;
	c->antenna_diversity = (c->nic[2] >> 8) & 1u;
	c->xtal_option = (c->nic[2] >> 9) & 3u;
	c->temperature_disabled = (c->nic[2] >> 11) & 1u;
	c->coex_method = (c->nic[2] >> 13) & 7u;
	c->country = e[0x39];
	if (c->country < 8u || c->country == 32u || c->country == 33u)
		c->valid_fields |= MT7601U_EE_VALID_COUNTRY;
	if (e[0x3a] != 0xffu && e[0xdb] != 0xffu) {
		c->frequency_offset = (int16_t)e[0x3a];
		if (e[0xdb] & 0x80u)
			c->frequency_offset -= e[0xdb] & 0x7fu;
		else
			c->frequency_offset += e[0xdb];
		c->valid_fields |= MT7601U_EE_VALID_FREQ;
	}
	c->lna_gain = (int8_t)e[0x44];
	if (e[0x44] != 0xffu)
		c->valid_fields |= MT7601U_EE_VALID_LNA;
	for (i = 0; i < 2u; i++)
		c->rssi_offset[i] = (int8_t)e[0x46u + i];
	if (e[0x46] != 0xffu && e[0x47] != 0xffu &&
	    c->rssi_offset[0] >= -10 && c->rssi_offset[0] <= 10 &&
	    c->rssi_offset[1] >= -10 && c->rssi_offset[1] <= 10)
		c->valid_fields |= MT7601U_EE_VALID_RSSI;
	for (i = 0; i < 14u; i++) {
		c->channel_power[i] = (int8_t)e[0x52u + i];
		if (e[0x52u + i] != 0xffu && c->channel_power[i] >= 0)
			c->channel_valid |= (uint16_t)(1u << i);
	}
	if (e[0x50] != 0xffu && (e[0x50] & 0x80u)) {
		c->bw40_delta = (int8_t)(e[0x50] & 0x1fu);
		if (c->bw40_delta > 8)
			c->bw40_delta = 8;
		if (e[0x50] & 0x40u)
			c->bw40_delta = -c->bw40_delta;
	}
	if (e[0x50] != 0xffu)
		c->valid_fields |= MT7601U_EE_VALID_BW40;
	for (i = 0; i < 20u; i++) {
		uint8_t v = e[0xdeu + i];
		c->rate_power[i] = (int8_t)((v & 0x3fu) - ((v & 0x20u) ? 64 : 0));
		if ((v & 0xc0u) == 0u)
			c->rate_valid |= 1u << i;
	}
	c->reference_temp = (int8_t)e[0xd1];
	if (e[0xd1] != 0xffu)
		c->valid_fields |= MT7601U_EE_VALID_TEMP;
	c->tssi_enabled = c->nic[1] != 0xffffu &&
		(c->nic[1] & (1u << 13)) != 0u && (c->nic[1] & 2u) == 0u;
	c->tssi_slope = e[0x6e];
	for (i = 0; i < 3u; i++)
		c->tssi_offset[i] = e[0x6fu + i];
	c->tssi_base_offset = e[0x76];
	c->tssi_target_power = e[0xd0];
	if (e[0x6e] != 0xffu && e[0x6f] != 0xffu && e[0x70] != 0xffu &&
	    e[0x71] != 0xffu && e[0x76] != 0xffu && e[0xd0] != 0xffu && e[0xd0] != 0u)
		c->valid_fields |= MT7601U_EE_VALID_TSSI;
}

enum mt7601u_efuse_status mt7601u_efuse_snapshot_init(
		struct mt7601u_efuse_snapshot *snapshot)
{
	uint32_t i, j;
	uint16_t address;
	uint8_t block[16];
	uint32_t saved_selection, control;
	enum mt7601u_efuse_status status;
	if (snapshot == (struct mt7601u_efuse_snapshot *)0)
		return MT7601U_EFUSE_INVALID;
	saved_selection = MT7601U_EFUSE_READ32(MT7601U_EFUSE_CTRL) &
		(MT7601U_EFUSE_AIN | MT7601U_EFUSE_MODE);
	/* Never restore a programming mode, even if the saved register was bad. */
	if ((saved_selection & MT7601U_EFUSE_MODE) > (1u << 6))
		saved_selection &= ~MT7601U_EFUSE_MODE;
	for (i = 0; i < sizeof(*snapshot); i++)
		((uint8_t *)snapshot)[i] = 0u;
	for (i = 0; i < MT7601U_EFUSE_BLOCKS; i++)
		snapshot->block_status[i] = MT7601U_EFUSE_NOT_READ;
	for (i = 0; i < MT7601U_EFUSE_BLOCKS; i++) {
		address = (uint16_t)(i < 16u ? i * 16u : 0x1e0u + (i - 16u) * 16u);
		status = mt7601u_efuse_read_block(i < 16u ? MT7601U_EFUSE_LOGICAL :
			MT7601U_EFUSE_PHYSICAL, address, block);
		snapshot->block_status[i] = (uint8_t)status;
		if (status != MT7601U_EFUSE_OK && status != MT7601U_EFUSE_UNMAPPED) {
			snapshot->status = (uint8_t)status;
			snapshot->error_address = address;
			goto finish;
		}
		for (j = 0; j < 16u; j++) {
			if (i < 16u)
				snapshot->logical[address + j] = block[j];
			else if ((i - 16u) * 16u + j < MT7601U_EFUSE_MAP_SIZE)
				snapshot->usage_map[(i - 16u) * 16u + j] = block[j];
		}
		if (i >= 16u && status == MT7601U_EFUSE_UNMAPPED) {
			snapshot->status = (uint8_t)status;
			snapshot->error_address = address;
			goto finish;
		}
	}
	snapshot->complete = 1u;
	mt7601u_efuse_decode(snapshot);
finish:
	/* Handoff only an idle controller. Preserve its unrelated live bits,
	 * restore the prior read selection and do not assert KICK. A timeout
	 * must leave the unfinished transaction alone for diagnosis. */
	control = MT7601U_EFUSE_READ32(MT7601U_EFUSE_CTRL);
	if ((control & MT7601U_EFUSE_KICK) == 0u) {
		control &= ~(MT7601U_EFUSE_AIN | MT7601U_EFUSE_MODE);
		MT7601U_EFUSE_WRITE32(MT7601U_EFUSE_CTRL, control | saved_selection);
	}
	return (enum mt7601u_efuse_status)snapshot->status;
}

const struct mt7601u_efuse_config *mt7601u_efuse_config_get(
		const struct mt7601u_efuse_snapshot *snapshot)
{
	return snapshot != (const struct mt7601u_efuse_snapshot *)0 && snapshot->complete ?
		&snapshot->config : (const struct mt7601u_efuse_config *)0;
}

void mt7601u_efuse_log(const struct mt7601u_efuse_snapshot *s)
{
	uint32_t i, j;
	const struct mt7601u_efuse_config *c = &s->config;
	for (i = 0; i < MT7601U_EFUSE_BLOCKS; i++) {
		uint32_t count = i < 17u ? 16u : 13u;
		const uint8_t *data = i < 16u ? s->logical + i * 16u :
			s->usage_map + (i - 16u) * 16u;
#if !INDEPENDENT_LOGGING
		uint32_t words[20];
		words[0] = i < 16u ? 0u : 1u;
		words[1] = i < 16u ? i * 16u : 0x1e0u + (i - 16u) * 16u;
		words[2] = s->block_status[i];
		words[3] = s->block_status[i] <= MT7601U_EFUSE_UNMAPPED ? count : 0u;
		for (j = 0; j < words[3]; j++) words[4+j] = data[j];
		mt7601u_evidence_emit(MT_EE_BLOCK, words, 4 + words[3]);
#else
		mt7601u_log_printf("efuse: %s %04x %u ", i < 16u ? "L" : "P",
			i < 16u ? i * 16u : 0x1e0u + (i - 16u) * 16u,
			(uint32_t)s->block_status[i]);
		if (s->block_status[i] <= MT7601U_EFUSE_UNMAPPED)
			for (j = 0; j < count; j++)
				mt7601u_log_printf("%02x", (uint32_t)data[j]);
		mt7601u_log_printf("\n");
#endif
	}
	MT7601U_REPORT(MT_EE_RESULT, "efuse: result complete=%u status=%u address=%04x\n",
		(uint32_t)s->complete, (uint32_t)s->status, (uint32_t)s->error_address);
	if (!s->complete)
		return;
#if !INDEPENDENT_LOGGING
	MT7601U_REPORT(MT_EE_IDENTITY, 0, c->chip_id, c->version_ee, c->version_fae,
		c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
#else
	mt7601u_log_printf("efuse: identity chip=%04x ee=%02x fae=%02x mac=",
		(uint32_t)c->chip_id, (uint32_t)c->version_ee, (uint32_t)c->version_fae);
	for (i = 0; i < 6u; i++)
		mt7601u_log_printf("%02x", (uint32_t)c->mac[i]);
	mt7601u_log_printf("\n");
#endif
	MT7601U_REPORT(MT_EE_CONFIG, "efuse: config valid=%08x nic=%04x,%04x,%04x country=%u freq=%d\n",
		c->valid_fields, (uint32_t)c->nic[0], (uint32_t)c->nic[1], (uint32_t)c->nic[2],
		(uint32_t)c->country, (int32_t)c->frequency_offset);
	MT7601U_REPORT(MT_EE_PHY, "efuse: phy lna=%d rssi=%d,%d temp=%d bw40=%d channel_valid=%04x rate_valid=%08x\n",
		(int32_t)c->lna_gain, (int32_t)c->rssi_offset[0], (int32_t)c->rssi_offset[1],
		(int32_t)c->reference_temp, (int32_t)c->bw40_delta,
		(uint32_t)c->channel_valid, c->rate_valid);
	MT7601U_REPORT(MT_EE_NIC, "efuse: nic byte_valid=%02x paths=%u,%u board=%u hw_rf=%u temp_alc=%u ext_lna=%u,%u tx_alc=%u streams=%u,%u antdiv=%u xtal=%u temp_disable=%u coex=%u\n",
		(uint32_t)c->nic_byte_valid, (uint32_t)c->rx_paths, (uint32_t)c->tx_paths,
		(uint32_t)c->board_type, (uint32_t)c->hw_rf_control,
		(uint32_t)c->temperature_alc, (uint32_t)c->external_lna_2g,
		(uint32_t)c->external_lna_5g, (uint32_t)c->tx_alc,
		(uint32_t)c->rx_streams, (uint32_t)c->tx_streams,
		(uint32_t)c->antenna_diversity, (uint32_t)c->xtal_option,
		(uint32_t)c->temperature_disabled, (uint32_t)c->coex_method);
	MT7601U_REPORT(MT_EE_TSSI, "efuse: tssi enabled=%u slope=%u offsets=%u,%u,%u base=%u target=%u\n",
		(uint32_t)c->tssi_enabled, (uint32_t)c->tssi_slope,
		(uint32_t)c->tssi_offset[0], (uint32_t)c->tssi_offset[1],
		(uint32_t)c->tssi_offset[2], (uint32_t)c->tssi_base_offset,
		(uint32_t)c->tssi_target_power);
#if !INDEPENDENT_LOGGING
	{ uint32_t words[20];
	  for (i=0;i<14;i++) words[i]=(int32_t)c->channel_power[i];
	  mt7601u_evidence_emit(MT_EE_CHANNEL, words, 14);
	  for (i=0;i<20;i++) words[i]=(int32_t)c->rate_power[i];
	  mt7601u_evidence_emit(MT_EE_RATE, words, 20);
	}
#else
	mt7601u_log_printf("efuse: channel power=");
	for (i = 0; i < 14u; i++)
		mt7601u_log_printf("%s%d", i ? "," : "", (int32_t)c->channel_power[i]);
	mt7601u_log_printf("\nefuse: rate power=");
	for (i = 0; i < 20u; i++)
		mt7601u_log_printf("%s%d", i ? "," : "", (int32_t)c->rate_power[i]);
	mt7601u_log_printf("\n");
#endif
}
