#include "mt7601u_bw_dpd.h"
#include "mt7601u_runtime.h"
#include "mt7601u_wow.h"
#include "mt7601u_calibration.h"
#include "mt7601u_iq_calibration.h"
#include "mt7601u_rf_bbp.h"
#include "mt7601u_hw.h"
#include "mt7601u_cmd.h"
#include "mt7601u_log_ring.h"

static void dma_barrier(void)
{
	__asm__ volatile ("" ::: "memory");
#ifdef __NDS32__
	__builtin_nds32_dsb();
#endif
}

/* FCE descriptors and command payloads are little-endian byte layouts.
 * Convert DMA bytes explicitly, independently of the selected C data ABI;
 * MMIO register accesses retain their native register values. */
static uint32_t load_le32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(volatile uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static uint16_t load_le16(const volatile uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void store_le16(volatile uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void clear_bytes(volatile uint8_t *bytes, uint32_t length)
{
	while (length-- != 0u)
		*bytes++ = 0;
}

static void init_free_ring(struct mt7601u_fce_ring *ring)
{
	uint32_t index;

	for (index = 0; index < MT7601U_FCE_RING_ENTRIES; index++) {
		clear_bytes(ring->desc[index], MT7601U_FCE_DESC_BYTES);
		ring->desc[index][7] = 0xC0u;
		ring->info[index] = (void *)0;
	}
}

static void init_buffer_ring(struct mt7601u_fce_ring *ring,
		uint8_t *payload, uint32_t stride, uint16_t buffer_length)
{
	uint32_t index;

	for (index = 0; index < MT7601U_FCE_RING_ENTRIES; index++) {
		clear_bytes(ring->desc[index], MT7601U_FCE_DESC_BYTES);
		ring->info[index] = payload + index * stride;
		store_le32(ring->desc[index],
			(uint32_t)(uintptr_t)ring->info[index]);
		store_le16(ring->desc[index] + 6, buffer_length);
	}
}

static int wait_pdma_idle(void)
{
	uint32_t count = 0x100000u;

	while ((FCE_PDMA_GLOBAL_CONF & MT7601U_FCE_PDMA_BUSY_MASK) != 0u) {
		if (--count == 0u)
			return 0;
	}
	return 1;
}

void mt7601u_fce_init(struct mt7601u_runtime *runtime)
{
	struct mt7601u_fce_state *state = &runtime->fce;
	uint32_t value;

	init_buffer_ring(&state->tx0, &state->tx0_payload[0][0],
		MT7601U_FCE_CMD_BYTES, MT7601U_FCE_TX0_BYTES);
	init_buffer_ring(&state->tx1, &state->tx1_payload[0][0],
		MT7601U_FCE_TX1_BYTES, MT7601U_FCE_TX1_BYTES);
	init_free_ring(&state->cpu_cmd);
	init_free_ring(&state->cmd_resp);
	dma_barrier();
	state->idx_tx0 = 0u;
	state->idx_tx1 = 0u;
	state->idx_cmd = 0u;
	state->idx_resp = 0u;

	/* This follows FUN_3bdc's recovered ordering, with ring storage in the
	 * independent runtime's verified ILM workspace. */
	FCE_PSE_CTRL |= 2u;
	FCE_PDMA_GLOBAL_CONF = 0x40000040u;
	if (!wait_pdma_idle()) {
		runtime->fault = 0xFCE00001u;
		return;
	}

	FCE_TX0_BASE = (uint32_t)(uintptr_t)state->tx0.desc;
	MT7601U_FCE_TX0_MAX = MT7601U_FCE_RING_ENTRIES;
	FCE_TX0_IDX8 = 0u;
	FCE_TX1_BASE = (uint32_t)(uintptr_t)state->tx1.desc;
	MT7601U_FCE_TX1_MAX = MT7601U_FCE_RING_ENTRIES;
	FCE_TX1_IDX8 = 0u;
	FCE_CPU_CMD_BASE = (uint32_t)(uintptr_t)state->cpu_cmd.desc;
	MT7601U_FCE_CPU_CMD_MAX = MT7601U_FCE_RING_ENTRIES;
	FCE_CPU_CMD_IDX8 = 0u;
	FCE_CPU_CMD_CPU_IDX8 = 0u;
	FCE_CMD_BASE = (uint32_t)(uintptr_t)state->cmd_resp.desc;
	MT7601U_FCE_CMD_MAX = MT7601U_FCE_RING_ENTRIES;
	FCE_CMD_CPU_IDX8 = 0u;
	FCE_CMD_CPU_IDX_HOST8 = 0u;

	/* FUN_3bdc: movi r3,1; sbi [0x9a8]; sbi [0x9b8].  Use the literal so the
	 * doorbell does not depend on runtime struct placement. */
	FCE_TX0_IDX8 = 1u;
	FCE_TX1_IDX8 = 1u;

	/* FUN_3bdc's low-byte patch is confirmed by the native post-init
	 * readback 0x40000045. Descriptor byte conversion is separate. */
	value = FCE_PDMA_GLOBAL_CONF;
	FCE_PDMA_GLOBAL_CONF = value | 5u;
	state->count_a8 = FCE_PDMA_GLOBAL_CONF;
	runtime->stage_mask |= MT7601U_STAGE_FCE;
}

static int response_ring_available(struct mt7601u_fce_state *state)
{
	uint8_t host = FCE_CMD_CPU_IDX_HOST8 & 1u;
	uint8_t next = (uint8_t)((state->idx_resp + 1u) & 1u);

	return next != host && (state->cmd_resp.desc[state->idx_resp][7] & 0x80u);
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t length)
{
	while (length-- != 0u)
		*dst++ = *src++;
}

static void send_command_response(struct mt7601u_runtime *runtime,
		uint32_t txinfo, uint32_t status, const uint8_t *payload,
		uint16_t payload_length)
{
	struct mt7601u_fce_state *state = &runtime->fce;
	uint8_t index;
	volatile uint8_t *desc;
	uint32_t response;
	uint16_t response_length = payload_length;
	uint32_t copy_length;
	uint32_t i;

	if (((txinfo >> 16) & 0xfu) == 0u)
		return;
	if (!response_ring_available(state)) {
		state->count_bad++;
		return;
	}

	if (response_length < 8u)
		response_length = 8u;
	if (response_length > MT7601U_FCE_CMD_BYTES - 8u) {
		state->count_bad++;
		return;
	}

	index = state->idx_resp;
	desc = state->cmd_resp.desc[index];
	response = (uint32_t)response_length | (txinfo & 0x000f0000u) |
		0x58000000u | ((status & 0xfu) << 20) |
		((uint32_t)(state->qsel & 3u) << 25);
	copy_length = payload_length;
	if (copy_length > response_length)
		copy_length = response_length;
	if (payload != (const uint8_t *)0)
		copy_bytes(state->response_payload[index], payload, copy_length);
	for (i = copy_length; i < response_length; i++)
		state->response_payload[index][i] = 0u;
	store_le32(desc, (uint32_t)(uintptr_t)state->response_payload[index]);
	store_le16(desc + 6, (uint16_t)((load_le16(desc + 6) & 0xc000u) |
			(response_length & 0x3fffu)));
	desc[7] = (uint8_t)((desc[7] & 0x3fu) | 0x40u);
	desc[5] &= 0x7fu;
	store_le32(desc + 12, response);
	dma_barrier();

	state->idx_resp = (uint8_t)((index + 1u) & 1u);
	FCE_CMD_CPU_IDX8 = state->idx_resp;
	state->count_responses++;
}

__attribute__((noinline))
static uint32_t run_command_body(struct mt7601u_runtime *runtime,
		uint32_t command, uint8_t *payload, uint16_t length)
{
	struct mt7601u_fce_state *state = &runtime->fce;
	uint32_t i;

	if (command == CMD_FUN_SET_OP) {
		uint32_t operation;
		uint32_t value;

		if (length != 8u)
			return 1u;
		operation = load_le32(payload);
		value = load_le32(payload + 4);
		if (operation == Q_SELECT) {
			state->qsel = (uint8_t)value;
			if (state->qsel >= 2u)
				state->qsel = 0;
		} else if (operation == ATOMIC_TSSI_SETTING) {
			return mt7601u_tssi_kick(value) ? 1u : 0u;
		} else if (operation == 8u) {
			state->bw_filter_mode = (uint8_t)value & 1u;
		} else {
			return 1u;
		}
		return 0u;
	}

	if (command == CMD_BURST_WRITE) {
		uint32_t address;

		if (length < 8u || (length & 3u) != 0u)
			return 1u;
		address = load_le32(payload);
		for (i = 1u; i < (uint32_t)length / 4u; i++, address += 4u) {
			uint32_t value = load_le32(payload + i * 4u);

			mt7601u_host_access_begin(runtime,
				MT7601U_HOST_OP_FCE_WRITE, address, 4u, value);
			if (!mt7601u_host_write32(runtime, address, value))
				return 1u;
		}
		return 0u;
	}

	if (command == CMD_RANDOM_WRITE) {
		if (length == 0u || (length & 7u) != 0u)
			return 1u;
		for (i = 0u; i < (uint32_t)length / 4u; i += 2u) {
			uint32_t address = load_le32(payload + i * 4u);
			uint32_t value = load_le32(payload + (i + 1u) * 4u);

			mt7601u_host_access_begin(runtime,
				MT7601U_HOST_OP_FCE_WRITE, address, 4u, value);
			if (!mt7601u_host_write32(runtime, address, value))
				return 1u;
		}
		return 0u;
	}

	if (command == CMD_RANDOM_READ) {
		if (length == 0u || (length & 7u) != 0u || length > 192u)
			return 1u;
		for (i = 0u; i < (uint32_t)length / 4u; i += 2u) {
			uint32_t address = load_le32(payload + i * 4u);

			if ((address & 3u) != 0u ||
				address < MT7601U_LOG_RING_BASE ||
				address > MT7601U_LOG_RING_END - 4u)
				return 1u;
			store_le32(payload + (i + 1u) * 4u, MMIO32(address));
		}
		return 0u;
	}

	if (command == CMD_CALIBRATION_OP) {
		uint32_t operation, value;

		if (length != 8u)
			return 1u;
		operation = load_le32(payload);
		value = load_le32(payload + 4);
		/* Linux has released reset and installed PHY tables before its first
		 * calibration. Report real shared reads once at that boundary. */
		mt7601u_rf_bbp_boot_report();
		if (operation == 1u || operation == 9u)
			return mt7601u_calibrate(operation) ? 1u : 0u;
		if (operation == 6u)
			return mt7601u_bw_calibrate(value, state->bw_filter_mode) ? 1u : 0u;
		if (operation == 7u)
			return mt7601u_dpd_calibrate(value) ? 1u : 0u;
		if (operation == 4u || operation == 5u || operation == 8u)
			return mt7601u_iq_calibrate(operation, value) ? 1u : 0u;
		return 1u;
	}
	if (command == CMD_LED_MODE_OP) {
		if (length != 8u && length != 16u)
			return 1u;
		for (i = 0; i < length; i += 8u)
			if (!mt7601u_led_mode_valid(load_le32(payload + i),
						    load_le32(payload + i + 4u)))
				return 1u;
		for (i = 0; i < length; i += 8u)
			mt7601u_led_set_mode(load_le32(payload + i),
					    load_le32(payload + i + 4u));
		return 0u;
	}
	if (command == CMD_WOW_CONFIG || command == CMD_WOW_FEATURE)
		return mt7601u_wow_command(command, payload, length);
	/* Bound repeated input-dependent output. Log only metadata. */
	if (state->unknown_reports < 8u) {
		state->unknown_reports++;
		mt7601u_log_event(MT_EVENT_UNKNOWN_COMMAND, command, length);
	}
	return 1u;
}

__attribute__((section(".portguard"), noinline))
static uint32_t run_command(struct mt7601u_runtime *runtime,
		uint32_t command, uint8_t *payload, uint16_t length)
{
	struct mt7601u_rf_bbp_ports ports;
	uint32_t status;
	int borrowed = length == 8u &&
		(command == CMD_CALIBRATION_OP ||
		 (command == CMD_FUN_SET_OP && load_le32(payload) == 5u));
	/* EP0 and FCE execute under the foreground GIE exclusion. A Linux
	 * direct CSR read or halfword pair can straddle this command boundary;
	 * restore its real completed controller word after calibration. */
	if (borrowed && mt7601u_rf_bbp_save_ports(&ports)) {
		mt7601u_log_printf("ports: save failed cmd=%u\n", command);
		return 1u;
	}
	status = run_command_body(runtime, command, payload, length);
	if (borrowed && mt7601u_rf_bbp_restore_ports(&ports)) {
		mt7601u_log_printf("ports: restore failed cmd=%u\n", command);
		return 1u;
	}
	return status;
}

static void service_command(struct mt7601u_runtime *runtime,
		uint8_t index, volatile uint8_t *desc)
{
	struct mt7601u_fce_state *state = &runtime->fce;
	uint32_t pointer = load_le32(desc);
	uint32_t txinfo;
	uint16_t length;
	uint8_t *payload;
	uint32_t status;
	uint8_t command;

	length = load_le16(desc + 6);
	length &= 0x3fffu;
	if (length > MT7601U_FCE_TX0_BYTES) {
		state->count_bad++;
		return;
	}
	if (pointer != (uint32_t)(uintptr_t)state->tx0_payload[index]) {
		state->count_bad++;
		return;
	}
	payload = (uint8_t *)(uintptr_t)pointer;
	txinfo = load_le32(desc + 12);
	state->last_txinfo = txinfo;
	if ((txinfo & MT7601U_TXINFO_TYPE_MASK) !=
			(MT7601U_DMA_COMMAND << MT7601U_TXINFO_TYPE_SHIFT)) {
		state->count_bad++;
		status = 1u;
	} else {
		command = mt7601u_txinfo_command_id(txinfo);
		status = run_command(runtime, command, payload, length);
	}
		send_command_response(runtime, txinfo, status, payload, length);
	state->count_commands++;
}

void mt7601u_fce_service(struct mt7601u_runtime *runtime)
{
	struct mt7601u_fce_state *state = &runtime->fce;
	uint8_t producer = FCE_TX0_CPU_IDX8 & 1u;
	uint8_t consumer = state->idx_tx0 & 1u;
	volatile uint8_t *desc;

	while (consumer != producer) {
		desc = state->tx0.desc[consumer];
		if ((desc[7] & 0x80u) == 0u)
			break;
		dma_barrier();
		service_command(runtime, consumer, desc);
		clear_bytes(desc, MT7601U_FCE_DESC_BYTES);
		store_le32(desc, (uint32_t)(uintptr_t)state->tx0_payload[consumer]);
		store_le16(desc + 6, MT7601U_FCE_TX0_BYTES);
		dma_barrier();
		/* Stock FUN_42bc returns this consumed slot to the FCE with its
		 * index, then advances its private consumer cursor. */
		FCE_TX0_IDX8 = consumer;
		consumer = (uint8_t)((consumer + 1u) & 1u);
		state->idx_tx0 = consumer;
		producer = FCE_TX0_CPU_IDX8 & 1u;
	}
	mt7601u_packet_receive(runtime);
}
