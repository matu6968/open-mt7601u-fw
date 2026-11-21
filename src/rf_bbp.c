#include "mt7601u_rf_bbp.h"
#include "mt7601u_mmio.h"
#ifdef __NDS32__
#include <nds32_intrinsic.h>
#endif

/* Separate hooks allow the compiled-C harness to model posted transactions. */
#ifndef RF_BBP_READ32
#define RF_BBP_READ32(address) MMIO32(address)
#define RF_BBP_WRITE32(address, value) (MMIO32(address) = (value))
#endif
#ifndef RF_BBP_IRQ_SAVE
static uint32_t irq_save(void)
{
#ifdef __NDS32__
	uint32_t psw = __nds32__mfsr(__NDS32_REG_PSW__);
	__nds32__setgie_dis();
	__nds32__dsb();
	__asm__ volatile ("" ::: "memory");
	return psw;
#else
	return 0u;
#endif
}
static void irq_restore(uint32_t psw)
{
#ifdef __NDS32__
	__asm__ volatile ("" ::: "memory");
	__nds32__dsb();
	if (psw & 1u)
		__nds32__setgie_en();
	else
		__nds32__setgie_dis();
	__nds32__dsb();
#else
	(void)psw;
#endif
}
#define RF_BBP_IRQ_SAVE() irq_save()
#define RF_BBP_IRQ_RESTORE(psw) irq_restore(psw)
#endif

static void poll_delay(void)
{
	unsigned n;
	for (n = 0; n < 64u; n++)
		__asm__ volatile ("nop");
}

enum mt7601u_rf_bbp_status mt7601u_poll32(uint32_t address, uint32_t mask,
	uint32_t expected, unsigned limit, uint32_t *last)
{
	unsigned n;
	uint32_t sample;
	if ((address & 3u) || !limit || limit > MT7601U_RF_BBP_LIMIT ||
	    (expected & ~mask))
		return MT7601U_RF_BBP_INVALID;
	for (n = 0; n < limit; n++) {
		sample = RF_BBP_READ32(address);
		if (last)
			*last = sample;
		if ((sample & mask) == expected)
			return MT7601U_RF_BBP_OK;
		if (n + 1u < limit)
			poll_delay();
	}
	return MT7601U_RF_BBP_POLL_TIMEOUT;
}

static int valid(unsigned bank, unsigned reg, int rf)
{
	return rf ? bank <= 15u && reg <= 63u : reg <= 255u;
}

enum mt7601u_rf_bbp_status __attribute__((section(".portguard")))
mt7601u_rf_bbp_save_ports(struct mt7601u_rf_bbp_ports *ports)
{
	struct mt7601u_rf_bbp_ports snapshot;
	if (!ports)
		return MT7601U_RF_BBP_INVALID;
	if (mt7601u_poll32(MT7601U_RF_PORT, MT7601U_RF_BUSY, 0,
			MT7601U_RF_BBP_LIMIT, &snapshot.rf) ||
	    mt7601u_poll32(MT7601U_BBP_PORT, MT7601U_BBP_BUSY, 0,
			MT7601U_RF_BBP_LIMIT, &snapshot.bbp))
		return MT7601U_RF_BBP_BUSY_TIMEOUT;
	*ports = snapshot;
	return MT7601U_RF_BBP_OK;
}

enum mt7601u_rf_bbp_status __attribute__((section(".portguard")))
mt7601u_rf_bbp_restore_ports(const struct mt7601u_rf_bbp_ports *ports)
{
	uint32_t sample;
	if (!ports || (ports->rf & MT7601U_RF_BUSY) ||
	    (ports->bbp & MT7601U_BBP_BUSY))
		return MT7601U_RF_BBP_INVALID;
	/* Check both ports before restoring either; stuck hardware is evidence
	 * of a failure, never a reason to overwrite its in-flight command. */
	if (mt7601u_poll32(MT7601U_RF_PORT, MT7601U_RF_BUSY, 0,
			MT7601U_RF_BBP_LIMIT, &sample) ||
	    mt7601u_poll32(MT7601U_BBP_PORT, MT7601U_BBP_BUSY, 0,
			MT7601U_RF_BBP_LIMIT, &sample))
		return MT7601U_RF_BBP_COMPLETION_TIMEOUT;
	RF_BBP_WRITE32(MT7601U_RF_PORT, ports->rf);
	RF_BBP_WRITE32(MT7601U_BBP_PORT, ports->bbp);
	return MT7601U_RF_BBP_OK;
}

/* Called with interrupts masked. Never start while busy and never retry a
 * mismatched completion: reporting it is safer than silently reissuing a
 * trigger. Preserve reserved bits, BBP PAR_DUR and the host's RW_MODE. */
static enum mt7601u_rf_bbp_status transaction(unsigned bank, unsigned reg,
	int rf, int write, uint8_t *value)
{
	uint32_t port = rf ? MT7601U_RF_PORT : MT7601U_BBP_PORT;
	uint32_t busy = rf ? MT7601U_RF_BUSY : MT7601U_BBP_BUSY;
	uint32_t fields = rf ? MT7601U_RF_FIELDS : MT7601U_BBP_FIELDS;
	uint32_t identity = (reg << 8) | (rf ? bank << 14 : 0u);
	uint32_t identity_mask = rf ? 0x3ff00u : 0xff00u;
	uint32_t control, command;
	if (mt7601u_poll32(port, busy, 0, MT7601U_RF_BBP_LIMIT, &control))
		return MT7601U_RF_BBP_BUSY_TIMEOUT;
	command = (control & ~fields) | identity | busy;
	if (rf ? write : !write)
		command |= rf ? MT7601U_RF_WRITE : MT7601U_BBP_READ;
	if (write)
		command |= *value;
	RF_BBP_WRITE32(port, command);
	if (mt7601u_poll32(port, busy, 0, MT7601U_RF_BBP_LIMIT, &control))
		return MT7601U_RF_BBP_COMPLETION_TIMEOUT;
	if ((control & identity_mask) != identity)
		return MT7601U_RF_BBP_MISMATCH;
	if (!write)
		*value = (uint8_t)(control & 0xffu);
	return MT7601U_RF_BBP_OK;
}

static enum mt7601u_rf_bbp_status access(unsigned bank, unsigned reg,
	int rf, int write, uint8_t *value)
{
	uint32_t psw;
	enum mt7601u_rf_bbp_status status;
	if (!value || !valid(bank, reg, rf))
		return MT7601U_RF_BBP_INVALID;
	psw = RF_BBP_IRQ_SAVE();
	status = transaction(bank, reg, rf, write, value);
	RF_BBP_IRQ_RESTORE(psw);
	return status;
}

enum mt7601u_rf_bbp_status mt7601u_rf_read(unsigned bank, unsigned reg, uint8_t *value)
{ return access(bank, reg, 1, 0, value); }
enum mt7601u_rf_bbp_status mt7601u_rf_write(unsigned bank, unsigned reg, uint8_t value)
{ return access(bank, reg, 1, 1, &value); }
enum mt7601u_rf_bbp_status mt7601u_bbp_read(unsigned reg, uint8_t *value)
{ return access(0, reg, 0, 0, value); }
enum mt7601u_rf_bbp_status mt7601u_bbp_write(unsigned reg, uint8_t value)
{ return access(0, reg, 0, 1, &value); }

static enum mt7601u_rf_bbp_status update(unsigned bank, unsigned reg, int rf,
	uint8_t mask, uint8_t value, uint8_t *updated)
{
	uint32_t psw;
	uint8_t before, after;
	enum mt7601u_rf_bbp_status status;
	if (!updated || !valid(bank, reg, rf))
		return MT7601U_RF_BBP_INVALID;
	psw = RF_BBP_IRQ_SAVE();
	status = transaction(bank, reg, rf, 0, &before);
	if (status == MT7601U_RF_BBP_OK) {
		after = (before & (uint8_t)~mask) | (value & mask);
		if (after != before)
			status = transaction(bank, reg, rf, 1, &after);
		if (status == MT7601U_RF_BBP_OK)
			*updated = after;
	}
	RF_BBP_IRQ_RESTORE(psw);
	return status;
}
enum mt7601u_rf_bbp_status mt7601u_rf_update(unsigned bank, unsigned reg,
	uint8_t mask, uint8_t value, uint8_t *updated)
{ return update(bank, reg, 1, mask, value, updated); }
enum mt7601u_rf_bbp_status mt7601u_bbp_update(unsigned reg,
	uint8_t mask, uint8_t value, uint8_t *updated)
{ return update(0, reg, 0, mask, value, updated); }

static enum mt7601u_rf_bbp_status poll(unsigned bank, unsigned reg, int rf,
	uint8_t mask, uint8_t expected, unsigned limit, uint8_t *last)
{
	unsigned n;
	uint8_t sample;
	enum mt7601u_rf_bbp_status status;
	if (!valid(bank, reg, rf) || !limit || limit > MT7601U_RF_BBP_LIMIT ||
	    (expected & (uint8_t)~mask))
		return MT7601U_RF_BBP_INVALID;
	for (n = 0; n < limit; n++) {
		status = access(bank, reg, rf, 0, &sample);
		if (status)
			return status;
		if (last)
			*last = sample;
		if ((sample & mask) == expected)
			return MT7601U_RF_BBP_OK;
		if (n + 1u < limit)
			poll_delay();
	}
	return MT7601U_RF_BBP_POLL_TIMEOUT;
}
enum mt7601u_rf_bbp_status mt7601u_rf_poll(unsigned bank, unsigned reg,
	uint8_t mask, uint8_t expected, unsigned limit, uint8_t *last)
{ return poll(bank, reg, 1, mask, expected, limit, last); }
enum mt7601u_rf_bbp_status mt7601u_bbp_poll(unsigned reg,
	uint8_t mask, uint8_t expected, unsigned limit, uint8_t *last)
{ return poll(0, reg, 0, mask, expected, limit, last); }
