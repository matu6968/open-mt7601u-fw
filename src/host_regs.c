#include "mt7601u_runtime.h"
#include "mt7601u_rf_bbp.h"
#include "mt7601u_log_ring.h"

static int address_in_range(uint32_t address, uint32_t base, uint32_t size)
{
	return address >= base && address < base + size;
}

static uint32_t diagnostic_value(uint32_t address, uint32_t value)
{
	/* Cover pairwise and shared key RAM, IV/EIV, and their attributes.
	 * This masks evidence only. The bus transaction still uses the
	 * original value. */
	if (address_in_range(address, MT_MCU_WLAN_BASE + 0x8000u, 0x3800u))
		return 0u;
	return value;
}

/* RANDOM_WRITE's high bits select serial BBP/RF operations. They are not
 * MCU memory windows (Imilab cmd_random_write, 0x20c4). */
static int indirect_address_valid(uint32_t address)
{
	if ((address & 0xffff0000u) == MT_MCU_BBP_BASE)
		return (address & 0xffffu) <= 255u;
	if ((address & 0xfff00000u) == MT_MCU_RF_BASE)
		return ((address >> 16) & 0xffu) <= 15u &&
			(address & 0xffffu) <= 63u;
	return 0;
}

static int indirect_write(uint32_t address, uint32_t value)
{
	enum mt7601u_rf_bbp_status status;
	if ((address & 0xc0000000u) == MT_MCU_BBP_BASE)
		status = mt7601u_bbp_write(address & 255u, (uint8_t)value);
	else
		status = mt7601u_rf_write((address >> 16) & 15u,
			address & 63u, (uint8_t)value);
	return status == MT7601U_RF_BBP_OK;
}

int mt7601u_host_address_valid(uint32_t address, uint8_t width)
{
	if (width == 2u) {
		if ((address & 1u) != 0u)
			return 0;
		return address_in_range(address, MT_MCU_WLAN_BASE, 0x10000u);
	}

	if (width == 4u && indirect_address_valid(address))
		return 1;
	if (width != 4u || (address & 3u) != 0u)
		return 0;
	return address_in_range(address, MT_MCU_SYS_BASE, 0x400u) ||
		address_in_range(address, MT_MCU_WLAN_BASE, 0x10000u);
}

void mt7601u_host_access_begin(struct mt7601u_runtime *runtime,
		uint8_t operation, uint32_t address, uint8_t width,
		uint32_t value)
{
	runtime->host_last_operation = operation;
	runtime->host_last_address = address;
	runtime->host_last_width = width;
	runtime->host_last_stage = MT7601U_HOST_STAGE_REQUEST;
	runtime->host_last_value = diagnostic_value(address, value);
	if (operation == MT7601U_HOST_OP_EP0_READ)
		runtime->host_read_count++;
	else if (operation == MT7601U_HOST_OP_EP0_WRITE ||
		 operation == MT7601U_HOST_OP_FCE_WRITE)
		runtime->host_write_count++;
	mt7601u_publish_transport_diagnostics(runtime);
}

void mt7601u_host_access_stage(struct mt7601u_runtime *runtime,
		uint8_t stage, uint32_t value)
{
	runtime->host_last_stage = stage;
	runtime->host_last_value = diagnostic_value(runtime->host_last_address, value);
	mt7601u_publish_transport_diagnostics(runtime);
}

void mt7601u_host_access_reject(struct mt7601u_runtime *runtime)
{
	runtime->host_reject_count++;
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_REJECTED,
		runtime->host_last_value);
}

int mt7601u_host_read32(struct mt7601u_runtime *runtime,
		uint32_t address, uint32_t *value)
{
	if (!mt7601u_host_address_valid(address, 4u) ||
		(address & 0xc0000000u) != 0u) {
		runtime->host_last_address = address;
		mt7601u_host_access_reject(runtime);
		return 0;
	}
	runtime->host_last_address = address;
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_MMIO,
		runtime->host_last_value);
	*value = MMIO32(address);
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_COMPLETE,
		*value);
	return 1;
}

int mt7601u_host_write16(struct mt7601u_runtime *runtime,
		uint32_t address, uint16_t value)
{
	if (!mt7601u_host_address_valid(address, 2u)) {
		runtime->host_last_address = address;
		mt7601u_host_access_reject(runtime);
		return 0;
	}
	runtime->host_last_address = address;
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_MMIO, value);
	MMIO16(address) = value;
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_COMPLETE, value);
	return 1;
}

int mt7601u_host_write32(struct mt7601u_runtime *runtime,
		uint32_t address, uint32_t value)
{
	if (!mt7601u_host_address_valid(address, 4u)) {
		runtime->host_last_address = address;
		mt7601u_host_access_reject(runtime);
		return 0;
	}
	runtime->host_last_address = address;
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_MMIO, value);
	if (indirect_address_valid(address)) {
		if (!indirect_write(address, value)) {
			mt7601u_host_access_reject(runtime);
			return 0;
		}
	} else {
		MMIO32(address) = value;
	}
	mt7601u_host_access_stage(runtime, MT7601U_HOST_STAGE_COMPLETE, value);
	return 1;
}
