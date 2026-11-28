#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t wlan[0x10000 / 4], ilm[0x10000 / 4], sys[0x400 / 4];

static volatile uint32_t *test_mmio(uint32_t address)
{
	assert(address < 0x10000u || (address >= 0x410000u && address < 0x420000u) ||
	       (address >= 0x400000u && address < 0x400400u));
	if (address >= 0x410000u)
		return &wlan[(address - 0x410000u) / 4];
	if (address >= 0x400000u)
		return &sys[(address - 0x400000u) / 4];
	return &ilm[address / 4];
}

#define MMIO32(address) (*test_mmio(address))
#define MMIO8(address) (*(volatile uint8_t *)test_mmio(address))
#define MMIO16(address) (*(volatile uint16_t *)((volatile uint8_t *)test_mmio(address) + ((address) & 2u)))
#define MT7601U_LOG_RING_BASE (0xd000u)

#include "../src/host_regs.c"
#include "../src/fce.c"
#include "../src/led.c"
#include "../src/mailbox.c"
#include "../src/wow.c"
#include "mt7601u_usb_desc.h"

static uint32_t calibration, tssi_value;

enum mt7601u_rf_bbp_status mt7601u_bbp_write(unsigned reg, uint8_t value)
{
	assert(reg == 47 && value == 17);
	return MT7601U_RF_BBP_OK;
}

enum mt7601u_rf_bbp_status mt7601u_rf_write(unsigned bank, unsigned reg, uint8_t value)
{
	assert(bank == 2 && reg == 7 && value == 3);
	return MT7601U_RF_BBP_OK;
}

void mt7601u_rf_bbp_boot_report(void) {}

unsigned mt7601u_calibrate(unsigned op)
{
	calibration = op;
	return 0;
}

unsigned mt7601u_iq_calibrate(unsigned op, uint32_t value)
{
	assert(value == 6);
	calibration = op;
	return 0;
}

unsigned mt7601u_bw_calibrate(uint32_t value, unsigned mode)
{
	assert(value == 6 && mode == 1);
	calibration = 6;
	return 0;
}

unsigned mt7601u_dpd_calibrate(uint32_t value)
{
	assert(value == 6);
	calibration = 7;
	return 0;
}

unsigned mt7601u_tssi_kick(uint32_t value)
{
	tssi_value = value;
	return 0;
}

static unsigned unknown_events, ready_events;

void mt7601u_log_event(enum mt7601u_log_event event, uint32_t first, uint32_t second)
{
	if (event == MT_EVENT_FIRMWARE_READY) {
		assert(first == 1 && second == 1);
		assert(WLAN_COM_REG0 == 1 && WLAN_COM_REG1 == 0x494e4450);
		ready_events++;
		return;
	}
	assert(event == MT_EVENT_UNKNOWN_COMMAND && first <= 127 && second == 8);
	unknown_events++;
}

static void test_mailbox(struct mt7601u_runtime *runtime)
{
	runtime->stage_mask = MT7601U_STAGE_USB | MT7601U_STAGE_FCE;
	runtime->usb_ep0_reads = 1;
	assert(!mt7601u_mailbox_publish_running(runtime));
	runtime->stage_mask |= MT7601U_STAGE_DMA;
	runtime->usb_ep0_reads = 0;
	assert(!mt7601u_mailbox_publish_running(runtime));
	assert(WLAN_COM_REG0 == 0 && WLAN_COM_REG1 == 0 && ready_events == 0);

	runtime->usb_ep0_reads = 1;
	assert(mt7601u_mailbox_publish_running(runtime));
	assert(runtime->stage_mask & MT7601U_STAGE_ALIVE);
	assert(!mt7601u_mailbox_publish_running(runtime));
	assert(ready_events == 1);
}

static void test_led(struct mt7601u_runtime *runtime)
{
	uint8_t payload[16] = {1};
	const uint32_t patterns[] = {0x10000, 0, 0, 0x32320001, 0x19190001,
				     0x11110001, 0x0a320001};

	SYS_REG(LED_MUX) = 0x12345678;
	for (unsigned mode = 0; mode <= 6; mode++) {
		store_le32(payload + 4, mode);
		assert(run_command_body(runtime, CMD_LED_MODE_OP, payload, 8) == 0);
		if (mode == 2) {
			assert(SYS_REG(LED_MUX) == 0x12348478);
			assert(SYS_REG(LED_ACTIVITY_01) == 0x03070307);
			assert(SYS_REG(LED_ACTIVITY_23) == 0x03070307);
		} else {
			assert(SYS_REG(LED_MUX) == 0x12348278);
			assert(SYS_REG(LED_PATTERN + 8) == patterns[mode]);
			assert(SYS_REG(LED_REPEAT + 8) == 0);
		}
	}
	store_le32(payload + 4, 0x10);
	store_le32(payload + 8, 4);
	assert(run_command_body(runtime, CMD_LED_MODE_OP, payload, 16) == 1);
	assert(SYS_REG(LED_PATTERN + 8) == 0x0a320001);
	store_le32(payload + 8, 3);
	assert(run_command_body(runtime, CMD_LED_MODE_OP, payload, 12) == 1);
	assert(run_command_body(runtime, CMD_LED_MODE_OP, payload, 16) == 0);
	assert(SYS_REG(LED_PATTERN + 8) == 0x11110190);
	assert(SYS_REG(LED_REPEAT + 8) == 0x01000001);
	assert(SYS_REG(LED_PATTERN + 24) == 0x10000);
	assert(SYS_REG(LED_MUX) == 0x82348278);
	store_le32(payload + 4, 7);
	assert(run_command_body(runtime, CMD_LED_MODE_OP, payload, 8) == 1);
	assert(SYS_REG(LED_MUX) == 0x82348278);
}

static void test_wowlan(struct mt7601u_runtime *runtime)
{
	uint8_t feature[8] = {3, 0, 0, 0, 1};
	uint8_t mask[16] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	uint8_t infra[20] = {3, 0, 0, 0, 2, 1, 2, 3, 4, 5, 2, 6, 7, 8, 9, 10, 1};
	uint8_t frame[26 + 2 + 8 + 102] = {0x88, 0x42};
	uint8_t *magic = frame + 26 + 2 + 8;
	uint32_t rxinfo = RX_PADDING | RX_DECRYPTED;
	uint32_t route = 0x83120005u;

	WLAN_REG(0x80c) = route;
	assert(run_command_body(runtime, CMD_WOW_FEATURE, feature, 8) == 1);
	feature[0] = 1;
	assert(run_command_body(runtime, CMD_WOW_FEATURE, feature, 8) == 0);
	assert(run_command_body(runtime, CMD_WOW_CONFIG, mask, 15) == 1);
	assert(run_command_body(runtime, CMD_WOW_CONFIG, mask, 16) == 0);
	assert(run_command_body(runtime, CMD_WOW_CONFIG, infra, 20) == 0);
	feature[0] = 4;
	assert(run_command_body(runtime, CMD_WOW_FEATURE, feature, 8) == 0);
	feature[0] = 3;
	assert(run_command_body(runtime, CMD_WOW_FEATURE, feature, 8) == 0);
	assert(WLAN_REG(0x80c) == ((route & ~FCE_ROUTE_MASK) | FCE_ROUTE_MCU));
	assert(mt7601u_wow_status() == 0x574f5702u);

	memcpy(frame + 4, infra + 4, 6);
	memcpy(frame + 10, infra + 10, 6);
	memset(magic, 0xff, 6);
	for (unsigned i = 1; i <= 16; i++)
		memcpy(magic + i * 6, infra + 4, 6);
	mt7601u_wow_suspend();
	assert(mt7601u_wow_receive(frame, sizeof(frame) - 2, rxinfo));
	mt7601u_wow_usb_arm(1);
	assert(mt7601u_wow_receive(frame, sizeof(frame) - 2, rxinfo));
	assert(!SYS_REG(0x80) && !USB_DMA_CFG);
	mt7601u_wow_suspend();
	assert(mt7601u_wow_receive(frame, sizeof(frame) - 3, rxinfo));
	assert(mt7601u_wow_receive(frame, sizeof(frame) - 2, RX_PADDING));
	frame[10] ^= 1;
	assert(mt7601u_wow_receive(frame, sizeof(frame) - 2, rxinfo));
	assert(!SYS_REG(0x80) && !USB_DMA_CFG);
	frame[10] ^= 1;
	assert(mt7601u_wow_receive(frame, sizeof(frame) - 2, rxinfo));
	assert(SYS_REG(0x80) == SYS_USB_WAKE_SIGNAL && USB_DMA_CFG == USB_WAKE_ENABLE);
	assert(mt7601u_wow_status() == 0x574f5703u);
	mt7601u_wow_usb_arm(0);
	assert(WLAN_REG(0x80c) == route && !SYS_REG(0x80));
	assert(mt7601u_wow_status() == 0x574f5701u);
	assert(!mt7601u_wow_receive(frame, sizeof(frame) - 2, rxinfo));
	feature[0] = 1;
	feature[4] = 0;
	assert(run_command_body(runtime, CMD_WOW_FEATURE, feature, 8) == 0);
	assert(mt7601u_wow_status() == 0x574f5700u);
}

static void test_usb_config_speed(void)
{
	/* Interface 0 of the stock config plus BootROM's second interface,
	 * which adds interrupt (0x81) and isochronous (0x83) endpoints that
	 * the ROM leaves at their table values. */
	static const uint8_t config[] = {
		0x09, 0x02, 0x3e, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x50,
		0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0xff, 0xff, 0x00,
		0x07, 0x05, 0x84, 0x02, 0x00, 0x02, 0x00,
		0x07, 0x05, 0x08, 0x02, 0x00, 0x02, 0x00,
		0x09, 0x04, 0x01, 0x00, 0x03, 0xff, 0xff, 0xff, 0x00,
		0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x00,
		0x07, 0x05, 0x82, 0x02, 0x00, 0x02, 0x00,
		0x07, 0x05, 0x83, 0x01, 0x40, 0x00, 0x00,
	};
	const uint32_t n = sizeof(config);
	uint8_t out[sizeof(config)];

	for (uint32_t i = 0; i < n; i++)
		assert(mt7601u_usb_config_byte(config, n, i, 0) == config[i]);
	for (uint32_t i = 0; i < n; i++)
		out[i] = mt7601u_usb_config_byte(config, n, i, 1);
	for (uint32_t i = 0; i < n; i++) {
		/* wMaxPacketSize low bytes of bulk endpoints 0x84, 0x08, 0x82 */
		int low = i == 22 || i == 29 || i == 52;
		int high = i == 23 || i == 30 || i == 53;

		if (low)
			assert(out[i] == 0x40);
		else if (high)
			assert(out[i] == 0x00);
		else
			assert(out[i] == config[i]);
	}
	/* A wLength-truncated request still patches what it sends. */
	assert(mt7601u_usb_config_byte(config, 23, 22, 1) == 0x40);
	assert(mt7601u_usb_config_byte(config, 22, 21, 1) == 0x02);
}

int main(void)
{
	struct mt7601u_runtime runtime = {0};
	uint8_t body[8] = {0};
	uint32_t address, value;

	test_mailbox(&runtime);
	test_led(&runtime);
	test_wowlan(&runtime);
	test_usb_config_speed();

	store_le32(body, Q_SELECT);
	store_le32(body + 4, 1);
	assert(run_command_body(&runtime, CMD_FUN_SET_OP, body, 8) == 0);
	assert(runtime.fce.qsel == 1);
	assert(run_command_body(&runtime, CMD_FUN_SET_OP, body, 7) == 1);
	store_le32(body, ATOMIC_TSSI_SETTING);
	store_le32(body + 4, 0x174u);
	assert(run_command_body(&runtime, CMD_FUN_SET_OP, body, 8) == 0);
	assert(tssi_value == 0x174u);
	store_le32(body, 8);
	store_le32(body + 4, 1);
	assert(run_command_body(&runtime, CMD_FUN_SET_OP, body, 8) == 0);
	store_le32(body, 7);
	assert(run_command_body(&runtime, CMD_FUN_SET_OP, body, 8) == 1);

	store_le32(body + 4, 6);
	for (unsigned op = 1; op <= 9; op++) {
		store_le32(body, op);
		calibration = 0;
		if (op == 2 || op == 3) {
			assert(run_command_body(&runtime, CMD_CALIBRATION_OP, body, 8) == 1);
			assert(calibration == 0);
		} else {
			assert(run_command_body(&runtime, CMD_CALIBRATION_OP, body, 8) == 0);
			assert(calibration == op);
		}
	}

	for (unsigned op = 0; op < 12; op++)
		assert(run_command_body(&runtime, CMD_WOW_CONFIG, body, 8) == 1);
	for (unsigned op = 0; op < 12; op++)
		assert(run_command_body(&runtime, CMD_WOW_QUERY, body, 8) == 1);
	assert(unknown_events == 8);

	store_le32(body, MT7601U_LOG_RING_BASE);
	MMIO32(MT7601U_LOG_RING_BASE) = 0xa5a5a5a5;
	assert(run_command_body(&runtime, CMD_RANDOM_READ, body, 8) == 0);
	assert(load_le32(body + 4) == 0xa5a5a5a5);
	store_le32(body, MT7601U_LOG_RING_END);
	assert(run_command_body(&runtime, CMD_RANDOM_READ, body, 8) == 1);

	store_le32(body, 0x418000u);
	store_le32(body + 4, 0x12345678u);
	assert(run_command_body(&runtime, CMD_BURST_WRITE, body, 8) == 0);
	assert(MMIO32(0x418000u) == 0x12345678u);
	assert(WLAN_COM_REG3 == 0 && runtime.host_last_value == 0);
	for (address = 0x418000u; address < 0x41b800u; address += 4u) {
		mt7601u_host_access_begin(&runtime, MT7601U_HOST_OP_EP0_WRITE, address + 2u, 2u, 0xbeefu);
		assert(WLAN_COM_REG3 == 0);
		assert(mt7601u_host_write16(&runtime, address + 2u, 0x5a5au));
		assert(MMIO16(address + 2u) == 0x5a5au);
		assert(WLAN_COM_REG3 == 0 && runtime.host_last_value == 0);
		assert(mt7601u_host_read32(&runtime, address, &value));
		assert(value == MMIO32(address));
		assert(WLAN_COM_REG3 == 0);
	}
	mt7601u_host_access_begin(&runtime, MT7601U_HOST_OP_EP0_WRITE, 0x411004u, 2u, 3u);
	assert(mt7601u_host_write16(&runtime, 0x411004u, 3u));
	assert(WLAN_COM_REG3 == 3u);

	memset(&runtime.fce, 0, sizeof(runtime.fce));
	send_command_response(&runtime, 0, 1, body, sizeof(body));
	assert(runtime.fce.count_responses == 0 && runtime.fce.count_bad == 0);
	runtime.fce.cmd_resp.desc[0][7] = 0xc0u;
	FCE_CMD_CPU_IDX_HOST8 = 0;
	send_command_response(&runtime, 0x10000u, 1, body, sizeof(body));
	assert(runtime.fce.count_responses == 1);
	assert((load_le32(runtime.fce.cmd_resp.desc[0] + 12) & 0x00ff0000u) == 0x00110000u);
	return 0;
}
