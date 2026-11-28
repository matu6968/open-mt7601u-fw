#include "mt7601u_runtime.h"
#include "mt7601u_wow.h"
#include "mt7601u_iq_calibration.h"
#include "mt7601u_log_ring.h"
#include "mt7601u_usb_desc.h"

#ifdef __NDS32__
#include <nds32_intrinsic.h>
#endif

#define USB_REQ_DEV_MODE	1u
#define USB_REQ_WRITE		2u
#define USB_REQ_MULTI_READ	7u
#define USB_REQ_VENDOR_TYPE	0x40u
#define USB_DIR_IN		0x80u
#define USB_REQ_GET_STATUS	0u
#define USB_REQ_CLEAR_FEATURE	1u
#define USB_REQ_SET_FEATURE	3u
#define USB_REQ_SET_ADDRESS	5u
#define USB_REQ_GET_DESCRIPTOR	6u
#define USB_REQ_GET_CONFIGURATION	8u
#define USB_REQ_SET_CONFIGURATION	9u
#define USB_REQ_GET_INTERFACE	10u
#define USB_REQ_SET_INTERFACE	11u

#define USB_STD_RECIPIENT_DEVICE	0x00u
#define USB_STD_RECIPIENT_INTERFACE	0x01u
#define USB_STD_RECIPIENT_ENDPOINT	0x02u
#define USB_FEATURE_ENDPOINT_HALT	0u
#define USB_FEATURE_REMOTE_WAKEUP	1u

#define USB_EP0_ARM_VALUE	2u

/* wValue the driver uses when it hands the firmware its IVB (mcu.c:387). */
#define USB_DEV_MODE_SET_IVB	0x12u
/* Largest payload any vendor request moves (FUN_4cc8 caps at 0x40 anyway). */
#define USB_EP0_MAX_XFER	0x40u
/* Bounded wait for an OUT data stage.  The stock firmware spins here without
 * a limit; a limit turns a missing data stage into a re-armable condition
 * instead of a wedged control endpoint. */
#define USB_EP0_OUT_SPIN	2000000u

static uint8_t usb_phy_inverted;
static uint8_t usb_phy_route_bad;
static uint8_t usb_phy_bank_select;
static uint8_t usb_phy_snapshot[0x30];
static uint32_t usb_shutdown_irq_mask;

/* Flags for usb_ep0_send_bytes(). */
#define USB_DESC_OTHER_SPEED		1	/* rewrite bDescriptorType to 7 */
#define USB_DESC_FULL_SPEED_BULK	2	/* bulk wMaxPacketSize reads 64 */

static void usb_ep0_complete(void);

/* USB+0x68C bit 0 is set only by the high-speed endpoint-map event
 * (USB+0x630 bit 5): BootROM bootrom_usb_set_endpoint_map (0x101160) sets it
 * together with speed flag 0xFCDA = 1 and the high-speed endpoint banks, and
 * mt7601u_usb_program_endpoints does the same here. The ROM's full-bank init
 * writes the register as 2 and its bus-reset handler leaves bit 0 alone, so on
 * a full-speed link it stays clear for the life of the attachment. */
static int usb_link_high_speed(void)
{
	return (USB_CTRL_168C & 1u) != 0u;
}

/* BootROM 0x10181c and Imilab 0x5e04 keep a high-address base for vendor
 * 0x20/0x23. Initialise explicitly because Andes small-common is separate
 * from the linker's ordinary BSS. */
static uint32_t usb_memory_base;

/* These are static descriptor defaults recovered at Imilab ILM 0xCA84.
 * The original PHY probe patches descriptor bytes at runtime (FUN_4d44).
 * These defaults can differ from BootROM's cached descriptors, so the first
 * runtime USB reset can replace the USB node during re-enumeration.
 * Keeping them as byte arrays matters: the NDS32 image is big-endian, but USB
 * descriptors are byte streams. */
static const uint8_t usb_device_descriptor[] = {
	0x12u, 0x01u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x40u,
	0x8Fu, 0x14u, 0x01u, 0x76u, 0x01u, 0x00u, 0x01u, 0x02u,
	0x03u, 0x01u,
};

static const uint8_t usb_device_qualifier_descriptor[] = {
	0x0Au, 0x06u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x40u,
	0x01u, 0x00u,
};

static const uint8_t usb_bos_descriptor[] = {
	0x05u, 0x0Fu, 0x0Cu, 0x00u, 0x01u,
	0x07u, 0x10u, 0x02u, 0x02u, 0x00u, 0x00u, 0x00u,
};

static const uint8_t usb_configuration_descriptor[] = {
	0x09u, 0x02u, 0x4Au, 0x00u, 0x01u, 0x01u, 0x00u, 0xA0u, 0x50u,
	0x09u, 0x04u, 0x00u, 0x00u, 0x08u, 0xFFu, 0xFFu, 0xFFu, 0x00u,
	0x07u, 0x05u, 0x84u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x85u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x08u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x04u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x05u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x06u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x07u, 0x02u, 0x00u, 0x02u, 0x00u,
	0x07u, 0x05u, 0x09u, 0x02u, 0x00u, 0x02u, 0x00u,
};

__attribute__((section(".image_usb.language"), aligned(1)))
static const uint8_t usb_string_language_descriptor[] = {
	0x04u, 0x03u, 0x09u, 0x04u,
};

__attribute__((section(".image_usb.manufacturer"), aligned(1)))
static const uint8_t usb_string_manufacturer_descriptor[] = {
	0x12u, 0x03u, 0x4Du, 0x00u, 0x65u, 0x00u, 0x64u, 0x00u,
	0x69u, 0x00u, 0x61u, 0x00u, 0x54u, 0x00u, 0x65u, 0x00u,
	0x6Bu, 0x00u,
};

__attribute__((section(".image_usb.product"), aligned(1)))
static const uint8_t usb_string_product_descriptor[] = {
	0x1Cu, 0x03u, 0x38u, 0x00u, 0x30u, 0x00u, 0x32u, 0x00u,
	0x2Eu, 0x00u, 0x31u, 0x00u, 0x31u, 0x00u, 0x20u, 0x00u,
	0x6Eu, 0x00u, 0x20u, 0x00u, 0x57u, 0x00u, 0x4Cu, 0x00u,
	0x41u, 0x00u, 0x4Eu, 0x00u,
};

__attribute__((section(".image_usb.serial"), aligned(1)))
static const uint8_t usb_string_serial_descriptor[] = {
	0x08u, 0x03u, 0x31u, 0x00u, 0x2Eu, 0x00u, 0x30u, 0x00u,
};

static void usb_phy_delay(uint32_t count)
{
	while (count-- != 0u)
		__asm__ volatile ("nop");
}

/* FUN_4d88 probes the USB PHY through the SYS 0x004 serial pins before it
 * programs the endpoint banks.  The access is deliberately kept local: it
 * is a hardware bring-up preamble, not a guessed WLAN register sequence. */
static void usb_phy_serial_begin(void)
{
	uint32_t value = SYS_REG(4u);

	value = (value & 0x1fdu) | 2u;
	SYS_REG(4u) = value;
	usb_phy_delay(1u);
	if (usb_phy_inverted != 0u)
		SYS_REG(4u) = value & 0x1fdu;
}

static void usb_phy_serial_end(void)
{
	uint32_t value;

	usb_phy_delay(1u);
	value = SYS_REG(4u) & 0x1fdu;
	if (usb_phy_inverted != 0u)
		value |= 2u;
	SYS_REG(4u) = value;
	usb_phy_delay(1u);
}

static void usb_phy_serial_write_byte(uint8_t value)
{
	uint32_t reg;
	uint32_t bit;

	for (bit = 0u; bit != 8u; bit++) {
		reg = SYS_REG(4u);
		reg = (reg & 0x1fbu) | ((uint32_t)(value >> 7) << 2);
		SYS_REG(4u) = reg;
		usb_phy_delay(1u);
		reg = (SYS_REG(4u) & 0x1feu) | 1u;
		SYS_REG(4u) = reg;
		usb_phy_delay(1u);
		SYS_REG(4u) = reg & 0x1feu;
		value <<= 1;
		usb_phy_delay(1u);
	}
	usb_phy_delay(10u);
}

static uint8_t usb_phy_serial_read_byte(void)
{
	uint32_t reg;
	uint32_t bit;
	uint8_t value = 0u;

	for (bit = 0u; bit != 8u; bit++) {
		usb_phy_delay(1u);
		reg = SYS_REG(4u);
		value = (uint8_t)((value << 1) | ((reg >> 3) & 1u));
		reg = (reg & 0x1feu) | 1u;
		SYS_REG(4u) = reg;
		usb_phy_delay(1u);
		SYS_REG(4u) = reg & 0x1feu;
	}
	usb_phy_delay(8u);
	return value;
}

static uint8_t usb_phy_serial_read(uint16_t address)
{
	uint8_t value;

	usb_phy_serial_begin();
	usb_phy_serial_write_byte(3u);
	if (usb_phy_inverted == 0u)
		address |= 0xfc00u;
	usb_phy_serial_write_byte((uint8_t)(address >> 8));
	usb_phy_serial_write_byte((uint8_t)address);
	value = usb_phy_serial_read_byte();
	usb_phy_serial_end();
	return value;
}

static void usb_phy_snapshot_read(uint8_t address, uint8_t count)
{
	uint8_t i;

	for (i = 0u; i != count; i++)
		usb_phy_snapshot[(uint8_t)(address + i)] =
			usb_phy_serial_read((uint16_t)(address + i));
}

static void __attribute__((unused)) mt7601u_usb_phy_init(void)
{
	uint32_t value;

	usb_phy_inverted = (SYS_REG(0x24u) & 0x80000000u) != 0u;
	value = usb_phy_serial_read(0x11u);
	if ((value & 0x0fu) == 2u) {
		if ((value & 0x10u) != 0u)
			SYS_REG(0x20u) &= ~0x800u;
		usb_phy_route_bad = 0u;
		/* FUN_4D88/FUN_4D44 read these groups before FUN_59DC changes
		 * endpoint banking.  Preserve the reads and their ordering even
		 * though the current runtime does not yet consume the snapshot. */
		usb_phy_snapshot_read(0x10u, 1u);
		usb_phy_snapshot_read(0x12u, 6u);
		usb_phy_snapshot_read(0x22u, 2u);
		usb_phy_snapshot_read(0x18u, 3u);
		usb_phy_snapshot_read(0x1Bu, 3u);
		usb_phy_snapshot_read(0x1Eu, 1u);
		usb_phy_snapshot_read(0x1Fu, 1u);
		usb_phy_snapshot_read(0x20u, 1u);
		usb_phy_snapshot_read(0x21u, 1u);
		usb_phy_snapshot_read(0x26u, 1u);
		usb_phy_snapshot_read(0x24u, 1u);
		usb_phy_snapshot_read(0x25u, 1u);
	} else {
		usb_phy_route_bad = 1u;
	}
}

static uint8_t usb_setup_byte(uint32_t offset)
{
	return (uint8_t)MMIO32(USB_SETUP_BASE + offset);
}

static uint16_t usb_setup_word(uint32_t offset)
{
	return (uint16_t)usb_setup_byte(offset) |
		((uint16_t)usb_setup_byte(offset + 4u) << 8);
}

static uint32_t __attribute__((unused)) usb_setup_signature(
		uint8_t request_type, uint8_t request,
		uint16_t value, uint16_t index, uint16_t length)
{
	return ((uint32_t)request_type | ((uint32_t)request << 8) |
		((uint32_t)value << 16)) ^ ((uint32_t)index << 3) ^
		((uint32_t)length << 19);
}

static int usb_fifo_push_host_bytes(struct mt7601u_runtime *runtime,
		uint32_t address, uint32_t count)
{
	uint32_t done = 0u;

	/* FUN_4c04 mode 0 reads aligned 32-bit words and sends each word
	 * least-significant byte first through 32-bit-spaced FIFO lanes. */
	while (done < count) {
		uint32_t source_word;
		uint32_t lane;

		if (address + done >= MT7601U_LOG_RING_BASE &&
			address + done <= MT7601U_LOG_RING_END - 4u) {
			source_word = MMIO32(address + done);
		} else if (address + done >= MT7601U_IRQ_DIAG_BASE &&
			address + done <= MT7601U_IRQ_DIAG_END - 4u) {
			source_word = MMIO32(address + done);
		} else if (address + done >= MT7601U_IQ_TRACE_BASE &&
			address + done <= MT7601U_IQ_TRACE_END - 4u) {
			source_word = MMIO32(address + done);
		} else if (address + done >= MT7601U_PACKET_DIAG_BASE &&
			address + done <= MT7601U_PACKET_DIAG_END - 4u) {
			source_word = MMIO32(address + done);
		} else
		if (!mt7601u_host_read32(runtime, address + done, &source_word)) {
			return 0;
		}
		for (lane = 0u; lane < 4u && done < count; lane++, done++)
			MMIO32(USB_EP0_FIFO_BASE + done * 4u) =
				source_word >> (lane * 8u);
	}
	runtime->usb_ep0_fifo_writes++;
	runtime->usb_ep0_last_fifo_addr = USB_EP0_FIFO_BASE;
	mt7601u_host_access_stage(runtime,
		MT7601U_HOST_STAGE_FIFO_LOADED, runtime->host_last_value);
	return 1;
}

static int usb_wlan_read_offset_valid(uint16_t offset)
{
	return (offset & 3u) == 0u;
}

static int usb_wlan_write_offset_valid(uint16_t offset)
{
	/* Linux sends each 32-bit write as halfwords at offset and offset+2. */
	return (offset & 1u) == 0u;
}

/* FUN_5ba0's host-to-device path reads one 32-bit RX FIFO lane per byte from
 * USB+0x500, takes that lane's low byte, and stores it sequentially in ILM. */
static void usb_fifo_pop(void *dest, uint32_t count)
{
	uint8_t *out = (uint8_t *)dest;
	uint32_t done = 0u;

	while (done < count) {
		out[done] = (uint8_t)MMIO32(USB_RX_FIFO + done * 4u);
		done++;
	}
}

/* Unlike the WLAN-register source helper above, descriptor bytes are already
 * in wire order in ILM.  The EP0 FIFO still takes one low-byte value per
 * 32-bit lane. */
static void usb_fifo_push_ram_bytes(const uint8_t *source, uint32_t count)
{
	uint32_t lane;

	for (lane = 0u; lane < count; lane++)
		MMIO32(USB_EP0_FIFO_BASE + lane * 4u) = source[lane];
}

static void usb_ep0_stall(void)
{
	/* Stock FUN_4b00 marks an unsupported standard request as a stall. */
	USB_FIFO_CONTROL = 1u;
	USB_REG(0x6A0u) = 0u;
}

static int usb_ep0_wait_tx_ready(void)
{
	uint32_t spins = USB_EP0_OUT_SPIN;

	/* FUN_4b44: a clear bit 2 means the EP0 FIFO can accept the next packet. */
	while ((((USB_FIFO_CONTROL ^ 4u) >> 2) & 1u) == 0u) {
		if (--spins == 0u)
			return 0;
	}
	return 1;
}

/* Configuration descriptor bytes for the current link speed. Same lane
 * format as usb_fifo_push_ram_bytes; length is the whole transfer so the
 * descriptor walk sees every header that precedes this packet. */
static void usb_fifo_push_config_bytes(const uint8_t *config, uint32_t length,
		uint32_t offset, uint32_t count)
{
	uint32_t lane;

	for (lane = 0u; lane < count; lane++)
		MMIO32(USB_EP0_FIFO_BASE + lane * 4u) =
			mt7601u_usb_config_byte(config, length, offset + lane, 1);
}

static int usb_ep0_send_bytes(struct mt7601u_runtime *runtime,
		const uint8_t *bytes, uint32_t length, int flags)
{
	uint32_t offset = 0u;

	if (length == 0u) {
		usb_ep0_complete();
		return 1;
	}
	while (offset < length) {
		uint32_t count = length - offset;

		if (count > USB_EP0_MAX_XFER)
			count = USB_EP0_MAX_XFER;
		if (!usb_ep0_wait_tx_ready())
			return 0;
		if ((flags & USB_DESC_FULL_SPEED_BULK) != 0)
			usb_fifo_push_config_bytes(bytes, length, offset, count);
		else
			usb_fifo_push_ram_bytes(bytes + offset, count);
		if ((flags & USB_DESC_OTHER_SPEED) != 0 && offset == 0u && count > 1u)
			MMIO32(USB_EP0_FIFO_BASE + 4u) = 7u;
		USB_TX_COUNT = count;
		USB_FIFO_CONTROL = USB_EP0_ARM_VALUE;
		runtime->usb_ep0_arms++;
		runtime->usb_ep0_last_fifo_addr = USB_EP0_FIFO_BASE;
		runtime->usb_ep0_last_tx_count = count;
		runtime->usb_ep0_last_control = USB_FIFO_CONTROL;
		runtime->usb_ep0_last_event_status = USB_EVENT_STATUS;
		offset += count;
	}
	return 1;
}

static volatile uint32_t *usb_endpoint_state(uint16_t address)
{
	uint32_t number = address & 0x0fu;
	uint32_t direction_in = (address & 0x80u) != 0u;
	uint32_t offset;

	if ((address & 0x70u) != 0u || number > 9u)
		return (volatile uint32_t *)0;
	if (number == 0u)
		return (volatile uint32_t *)0;
	if (direction_in != 0u) {
		if (number > 5u)
			return (volatile uint32_t *)0;
		offset = 0x018u + number * 0x20u;
	} else {
		offset = 0x008u + number * 0x20u;
	}
	return (volatile uint32_t *)(uintptr_t)(MT_MCU_USB_BASE + offset);
}

static int usb_endpoint_halted(uint16_t address)
{
	volatile uint32_t *state = usb_endpoint_state(address);

	if (state == (volatile uint32_t *)0)
		return address == 0u ? 0 : -1;
	return ((*state >> 6) & 1u) != 0u;
}

static int usb_set_endpoint_halt(uint16_t address, int halted)
{
	volatile uint32_t *state = usb_endpoint_state(address);
	uint32_t endpoint = address & 0x0fu;
	uint32_t command;
	uint32_t value;

	if (state == (volatile uint32_t *)0)
		return 0;
	command = endpoint | ((address & 0x80u) != 0u ? 0x10u : 0u);
	USB_REG(0x688u) = command;
	USB_REG(0x688u) = command | 0x60u;
	value = *state;
	if (halted != 0)
		value |= 0x40u;
	else
		value &= ~0x40u;
	*state = value;
	USB_REG(0x688u) = command;
	USB_REG(0x688u) = command | 0x60u;
	return 1;
}

static const uint8_t *usb_get_descriptor(uint16_t value, uint16_t index,
		uint32_t *length, int *flags)
{
	uint8_t type = (uint8_t)(value >> 8);
	uint8_t descriptor_index = (uint8_t)value;

	*flags = 0;
	switch (type) {
	case 1u:
		if (descriptor_index != 0u)
			return (const uint8_t *)0;
		*length = sizeof(usb_device_descriptor);
		return usb_device_descriptor;
	case 2u:
		if (descriptor_index != 0u)
			return (const uint8_t *)0;
		*length = sizeof(usb_configuration_descriptor);
		if (!usb_link_high_speed())
			*flags = USB_DESC_FULL_SPEED_BULK;
		return usb_configuration_descriptor;
	case 3u:
		if (descriptor_index == 0u) {
			*length = sizeof(usb_string_language_descriptor);
			return usb_string_language_descriptor;
		}
		if (index != 0u && index != 0x0409u)
			return (const uint8_t *)0;
		if (descriptor_index == 1u) {
			*length = sizeof(usb_string_manufacturer_descriptor);
			return usb_string_manufacturer_descriptor;
		}
		if (descriptor_index == 2u) {
			*length = sizeof(usb_string_product_descriptor);
			return usb_string_product_descriptor;
		}
		if (descriptor_index == 3u) {
			*length = sizeof(usb_string_serial_descriptor);
			return usb_string_serial_descriptor;
		}
		return (const uint8_t *)0;
	case 6u:
		if (descriptor_index != 0u)
			return (const uint8_t *)0;
		*length = sizeof(usb_device_qualifier_descriptor);
		return usb_device_qualifier_descriptor;
	case 7u:
		if (descriptor_index != 0u)
			return (const uint8_t *)0;
		*length = sizeof(usb_configuration_descriptor);
		/* The other-speed view of a high-speed link is full speed. */
		*flags = USB_DESC_OTHER_SPEED;
		if (usb_link_high_speed())
			*flags |= USB_DESC_FULL_SPEED_BULK;
		return usb_configuration_descriptor;
	case 15u:
		if (descriptor_index != 0u)
			return (const uint8_t *)0;
		*length = sizeof(usb_bos_descriptor);
		return usb_bos_descriptor;
	default:
		return (const uint8_t *)0;
	}
}

static int usb_ep0_standard_request(struct mt7601u_runtime *runtime)
{
	uint8_t request_type = runtime->usb_last_type;
	uint8_t request = runtime->usb_last_request;
	uint16_t value = runtime->usb_last_value;
	uint16_t index = runtime->usb_last_index;
	uint16_t length = runtime->usb_last_length;
	uint8_t response[2];
	const uint8_t *descriptor;
	uint32_t descriptor_length;
	uint32_t status;
	int descriptor_flags;

	runtime->usb_standard_requests++;
	switch (request) {
	case USB_REQ_GET_STATUS:
		if (request_type == 0x80u) {
			status = (USB_CTRL_168C & 8u) != 0u ? 2u : 0u;
		} else if (request_type == 0x81u) {
			status = 0u;
		} else if (request_type == 0x82u) {
			int halted = usb_endpoint_halted(index);

			if (halted < 0)
				goto unsupported;
			status = (uint32_t)halted;
		} else {
			goto unsupported;
		}
		response[0] = (uint8_t)status;
		response[1] = (uint8_t)(status >> 8);
		if (length > 2u)
			length = 2u;
		if (!usb_ep0_send_bytes(runtime, response, length, 0))
			goto not_ready;
		return 1;
	case USB_REQ_CLEAR_FEATURE:
	case USB_REQ_SET_FEATURE:
		if (request_type == USB_STD_RECIPIENT_DEVICE &&
			value == USB_FEATURE_REMOTE_WAKEUP && index == 0u &&
			length == 0u) {
			if (request == USB_REQ_SET_FEATURE) {
				USB_CTRL_168C |= 8u;
				USB_DMA_CFG |= 0x20000u;
			} else {
				USB_CTRL_168C &= ~8u;
				USB_DMA_CFG &= ~0x20000u;
			}
			mt7601u_wow_usb_arm(request == USB_REQ_SET_FEATURE);
			usb_ep0_complete();
			return 1;
		}
		if (request_type == USB_STD_RECIPIENT_ENDPOINT &&
			value == USB_FEATURE_ENDPOINT_HALT && length == 0u &&
			usb_set_endpoint_halt(index,
				request == USB_REQ_SET_FEATURE)) {
			usb_ep0_complete();
			return 1;
		}
		goto unsupported;
	case USB_REQ_SET_ADDRESS:
		/* The stock table entry is a no-op; the USB block latches the address. */
		if (request_type != 0x00u || value > 127u || index != 0u || length != 0u)
			goto unsupported;
		usb_ep0_complete();
		return 1;
	case USB_REQ_GET_DESCRIPTOR:
		if (request_type != 0x80u)
			goto unsupported;
		descriptor = usb_get_descriptor(value, index, &descriptor_length,
				&descriptor_flags);
		if (descriptor == (const uint8_t *)0)
			goto unsupported;
		if (descriptor_length > length)
			descriptor_length = length;
		if (!usb_ep0_send_bytes(runtime, descriptor, descriptor_length,
				descriptor_flags))
			goto not_ready;
		return 1;
	case USB_REQ_GET_CONFIGURATION:
		if (request_type != 0x80u || index != 0u || length == 0u)
			goto unsupported;
		response[0] = runtime->usb_configuration;
		if (!usb_ep0_send_bytes(runtime, response, 1u, 0))
			goto not_ready;
		return 1;
	case USB_REQ_SET_CONFIGURATION:
		if (request_type != 0x00u || value > 1u || index != 0u || length != 0u)
			goto unsupported;
		runtime->usb_configuration = (uint8_t)value;
		usb_ep0_complete();
		return 1;
	case USB_REQ_GET_INTERFACE:
		if (request_type != 0x81u || index != 0u || length == 0u)
			goto unsupported;
		response[0] = 0u;
		if (!usb_ep0_send_bytes(runtime, response, 1u, 0))
			goto not_ready;
		return 1;
	case USB_REQ_SET_INTERFACE:
		if (request_type != 0x01u || value != 0u || index != 0u || length != 0u)
			goto unsupported;
		usb_ep0_complete();
		return 1;
	default:
		goto unsupported;
	}

unsupported:
	runtime->usb_standard_errors++;
	usb_ep0_stall();
	return 0;
not_ready:
	runtime->usb_ep0_retry++;
	runtime->usb_standard_errors++;
	usb_ep0_complete();
	return 0;
}

/* Wait for the OUT data stage to land, exactly as FUN_5d50 and FUN_5ba0 do:
 * clear the byte counter, spin on USB 0x628 bit 0, then read the count back
 * out of USB 0x000.  Returns 0 if the stage never arrives, so the caller
 * re-arms instead of stalling. */
static uint32_t usb_ep0_wait_out_data(uint32_t spin_limit)
{
	uint32_t spins = spin_limit;

	USB_RX_COUNT_WRITE = 0u;
	while ((USB_EVENT_STATUS & 1u) == 0u) {
		if (--spins == 0u)
			return 0u;
	}
	USB_EVENT_STATUS = 1u;
	return (uint32_t)(USB_RX_COUNT & 0x3fu);
}

static void usb_ep0_complete(void)
{
	/* FUN_4b24 closes the control FIFO for the ordinary and multi-read paths;
	 * the caller has already written the transfer count when appropriate. */
	USB_FIFO_CONTROL = USB_EP0_ARM_VALUE;
}

static void __attribute__((unused)) usb_ep0_complete_read(void)
{
	/* fce_reset_goto_rom falls through FUN_4b24 after FUN_5c58, so the
	 * successful read also arms the control FIFO. */
	USB_FIFO_CONTROL = USB_EP0_ARM_VALUE;
}

static uint32_t usb_ep0_save_and_disable_gie(void)
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

static void usb_ep0_restore_gie(uint32_t saved_psw)
{
#ifdef __NDS32__
	__asm__ volatile ("" ::: "memory");
	/* Drain EP0/FCE acknowledgements before an IRQ can observe them.
	 * A barrier after SETGIE alone leaves an interrupt window with posted
	 * command/gate writes from the foreground transaction still pending. */
	__nds32__dsb();
	if ((saved_psw & 1u) != 0u)
		__nds32__setgie_en();
	else
		__nds32__setgie_dis();
	__nds32__dsb();
#else
	(void)saved_psw;
#endif
}

static void usb_ep0_return(uint32_t saved_psw)
{
	/* Imilab 0x5834 releases its temporary gate once, after the request
	 * handler has finished arming/completing the FIFO. Releasing early
	 * admits the next SETUP while the old foreground handler still runs. */
	USB_REG(0x6A0) = 0u;
	usb_ep0_restore_gie(saved_psw);
}

void mt7601u_usb_ep0_service(struct mt7601u_runtime *runtime)
{
	uint32_t saved_psw = usb_ep0_save_and_disable_gie();
	uint8_t request_type = runtime->usb_last_type;
	uint8_t request = runtime->usb_last_request;
	uint16_t value = runtime->usb_last_value;
	uint16_t index = runtime->usb_last_index;
	uint16_t length = runtime->usb_last_length;
	uint32_t transfer_length;
	uint32_t arm_value = USB_EP0_ARM_VALUE;

	runtime->usb_ep0_requests++;
	runtime->usb_setup_pending = 0u;

	if ((request_type & 0x60u) == USB_REQ_VENDOR_TYPE &&
		request != USB_REQ_DEV_MODE) {
		uint32_t host_address = MT_MCU_WLAN_BASE + index;
		uint8_t operation = MT7601U_HOST_OP_UNSUPPORTED;
	uint8_t width = 0u;

		if (request == USB_REQ_MULTI_READ &&
			(request_type & USB_DIR_IN) != 0u) {
			operation = MT7601U_HOST_OP_EP0_READ;
			width = 4u;
		} else if (request == USB_REQ_WRITE &&
			(request_type & USB_DIR_IN) == 0u) {
			operation = MT7601U_HOST_OP_EP0_WRITE;
			width = 2u;
		}
		mt7601u_host_access_begin(runtime, operation, host_address, width, request);
	}

	/* FUN_5834 owns the temporary USB control gate; FUN_5650 only captures
	 * the setup packet and raises the pending flag for this later phase. */
	USB_REG(0x6A0) = 0x80u;

	/* FUN_5834 switches on bmRequestType & 0x60, which selects the transfer
	 * type rather than the direction: 0x40 is vendor in both directions and
	 * goes to the FCE command path, 0x00 is standard and has its own table,
	 * 0x20 is class.  Stalling standard requests here would break
	 * re-enumeration once the ILM is loaded. */
	switch (request_type & 0x60u) {
	case 0x00u:	/* standard: SET_ADDRESS / SET_CONFIGURATION and friends */
		if (!usb_ep0_standard_request(runtime))
			runtime->usb_ep0_errors++;
		mt7601u_publish_transport_diagnostics(runtime);
		usb_ep0_return(saved_psw);
		return;
	case 0x20u:	/* class: FUN_5834 falls through to FUN_4b00 */
		mt7601u_host_access_begin(runtime,
			MT7601U_HOST_OP_UNSUPPORTED, index, 0u, request);
		mt7601u_host_access_reject(runtime);
		USB_FIFO_CONTROL = 1u;
		usb_ep0_return(saved_psw);
		return;
	case USB_REQ_VENDOR_TYPE:
		break;
	default:
		runtime->usb_ep0_errors++;
		mt7601u_host_access_begin(runtime,
			MT7601U_HOST_OP_UNSUPPORTED, index, 0u, request);
		mt7601u_host_access_reject(runtime);
		usb_ep0_stall();
		usb_ep0_return(saved_psw);
		return;
	}

	if (request == 0x20u && (request_type & USB_DIR_IN) == 0u && length == 0u) {
		usb_memory_base = (uint32_t)index << 16;
		usb_ep0_complete();
		usb_ep0_return(saved_psw);
		return;
	}
	if ((request == USB_REQ_MULTI_READ || request == 0x23u) &&
		(request_type & USB_DIR_IN) != 0u) {
		uint32_t host_address = (request == 0x23u ? usb_memory_base :
			MT_MCU_WLAN_BASE) + index;
		uint32_t count = length > USB_EP0_MAX_XFER ? USB_EP0_MAX_XFER : length;
		int log_read = host_address >= MT7601U_LOG_RING_BASE &&
			host_address <= MT7601U_LOG_RING_END - count;
		int irq_read = host_address >= MT7601U_IRQ_DIAG_BASE &&
			host_address <= MT7601U_IRQ_DIAG_END - count;

		/* Existing read-only 0x23 ABI, limited to the bounded IQ evidence
		 * region. It is outside runtime state, logs, IRQs and the stack. */
		log_read |= host_address >= MT7601U_IQ_TRACE_BASE &&
			host_address <= MT7601U_IQ_TRACE_END - count;
		log_read |= host_address >= MT7601U_PACKET_DIAG_BASE &&
			host_address <= MT7601U_PACKET_DIAG_END - count;
		if (length < 4u || !usb_wlan_read_offset_valid(index) ||
			(!log_read && !irq_read && (!mt7601u_host_address_valid(host_address, 4u) ||
			 !mt7601u_host_address_valid(host_address + ((count - 1u) & ~3u), 4u)))) {
			runtime->usb_ep0_errors++;
			mt7601u_host_access_reject(runtime);
			usb_ep0_stall();
			usb_ep0_return(saved_psw);
			return;
		}
		if ((uint32_t)index + count > 0x10000u) {
			runtime->usb_ep0_errors++;
			mt7601u_host_access_reject(runtime);
			usb_ep0_stall();
			usb_ep0_return(saved_psw);
			return;
		}
		mt7601u_host_access_stage(runtime,
			MT7601U_HOST_STAGE_VALIDATED, 0u);
		/* FUN_4b44 is the readiness gate.  fce_reset_goto_rom consults it
		 * and, when it is not ready, sets a retry flag and falls through to
		 * FUN_4b24 -- it never stalls.  Stalling the control endpoint is
		 * what the host reports as -EPROTO, so re-arm and be retried. */
		if ((((USB_FIFO_CONTROL ^ 4u) >> 2) & 1u) == 0u) {
			mt7601u_host_access_stage(runtime,
				MT7601U_HOST_STAGE_WAIT_READY, 0u);
			runtime->usb_ep0_retry++;
			/* Stock stores the not-ready result in the pending flag,
			 * acknowledges the setup, and still falls through to
			 * FUN_4b24.  Leaving the pending flag clear is the direct
			 * cause of the repeated -ETIMEDOUT read. */
			runtime->usb_setup_pending = 1u;
			USB_CMD_630 = 1u;
			usb_ep0_complete();
			usb_ep0_return(saved_psw);
			return;
		}
		/* FUN_5c58: count = min(wLength, 0x40), FUN_4c04 into USB+0x400,
		 * then USB_TX_COUNT, then FUN_4b24 arms the control FIFO.
		 *
		 * INDEPENDENT_EP0_TUNE sweeps the parts of that sequence that
		 * Ghidra has not yet pinned down -- which register the engine
		 * takes the IN length from, and which value arms the stage --
		 * because a short data stage is indistinguishable from every
		 * other failure once it reaches the host as -EPROTO.
		 *   0 = FUN_5c58 as transcribed (len 0x004, arm 2)
		 *   1 = also set 0x000
		 *   2 = len in 0x000 only
		 *   3 = len 0x004, arm 1 */
		transfer_length = length;
		if (transfer_length > USB_EP0_MAX_XFER)
			transfer_length = USB_EP0_MAX_XFER;
		if (!usb_fifo_push_host_bytes(runtime, host_address,
				transfer_length)) {
			runtime->usb_ep0_errors++;
			usb_ep0_complete();
			usb_ep0_return(saved_psw);
			return;
		}
		USB_TX_COUNT = transfer_length;
		USB_FIFO_CONTROL = arm_value;
		mt7601u_host_access_stage(runtime,
			MT7601U_HOST_STAGE_ARMED, runtime->host_last_value);
		/* fce_reset_goto_rom falls through FUN_4b24 before FUN_5834
		 * clears the temporary control gate. */
		runtime->usb_ep0_last_event_status = USB_EVENT_STATUS;
		runtime->usb_ep0_last_tx_count = USB_TX_COUNT;
		runtime->usb_ep0_last_control = USB_FIFO_CONTROL;
		runtime->usb_ep0_arms++;
		runtime->usb_ep0_reads++;
		if (mt7601u_mailbox_publish_running(runtime))
			mt7601u_usb_debug_dump(runtime, "alive");
		/* The data-stage branch already performed the stock completion;
		 * never arm the IN FIFO a second time. */
		usb_ep0_return(saved_psw);
		return;
	}

	if (request == 0x2fu && request_type == 0xc0u && !value && !index &&
	    (length == 4u || length == 8u)) {
		uint32_t status = mt7601u_wow_status();
		uint32_t rx = mt7601u_wow_rx_count();
		uint8_t bytes[] = { status, status >> 8, status >> 16, status >> 24,
			rx, rx >> 8, rx >> 16, rx >> 24 };

		if (!usb_ep0_send_bytes(runtime, bytes, length, 0))
			usb_ep0_stall();
		usb_ep0_return(saved_psw);
		return;
	}

	if (request == USB_REQ_WRITE && (request_type & USB_DIR_IN) == 0u) {
		uint32_t host_address = MT_MCU_WLAN_BASE + index;

		if (!usb_wlan_write_offset_valid(index)
			|| length != 0u
			) {
			runtime->usb_ep0_errors++;
			mt7601u_host_access_reject(runtime);
			usb_ep0_stall();
			usb_ep0_return(saved_psw);
			return;
		}
		mt7601u_host_access_stage(runtime,
			MT7601U_HOST_STAGE_VALIDATED, value);
		if (!mt7601u_host_write16(runtime, host_address, value)) {
			runtime->usb_ep0_errors++;
			usb_ep0_return(saved_psw);
			return;
		}
		usb_ep0_complete();
		usb_ep0_return(saved_psw);
		return;
	}

	/* MT_VEND_DEV_MODE.  wValue 0x12 carries the IVB upload: the driver
	 * writes our vector table into the ILM base so a reset lands on
	 * _start.  This is not a zero-length request, and the runtime has to be
	 * able to take the payload off the control endpoint or the firmware
	 * never receives its own vectors. */
	if (request == USB_REQ_DEV_MODE && (request_type & USB_DIR_IN) == 0u) {
		if (value == USB_DEV_MODE_SET_IVB && length != 0u) {
			uint32_t count = usb_ep0_wait_out_data(USB_EP0_OUT_SPIN);

			if (count == 0u) {
				/* Data stage has not landed.  Re-arm rather than
				 * stall, so the driver can retry the upload. */
				runtime->usb_ep0_retry++;
				USB_CMD_630 = 1u;
				usb_ep0_return(saved_psw);
				return;
			}
			if (count > length)
				count = length;
			if (count > USB_EP0_MAX_XFER)
				count = USB_EP0_MAX_XFER;
			usb_fifo_pop((void *)(uintptr_t)MT_MCU_ILM_BASE, count);
			runtime->usb_ep0_writes++;
		}
		usb_ep0_complete();
		usb_ep0_return(saved_psw);
		return;
	}

	runtime->usb_ep0_errors++;
	mt7601u_host_access_begin(runtime, MT7601U_HOST_OP_UNSUPPORTED,
		index, 0u, request);
	mt7601u_host_access_reject(runtime);
	usb_ep0_stall();
	usb_ep0_return(saved_psw);
}

static void __attribute__((unused)) mt7601u_usb_capture_setup(
		struct mt7601u_runtime *runtime,
		uint32_t signature)
{
	uint8_t request_type = usb_setup_byte(0u);
	uint8_t request = usb_setup_byte(4u);
	uint16_t value = usb_setup_word(8u);
	uint16_t index = usb_setup_word(16u);
	uint16_t length = usb_setup_word(24u);

	/* This is stock FUN_5650: capture fields, acknowledge setup capture, and
	 * let the scheduler/next poll perform the request-specific transaction. */
	(void)signature;
	runtime->usb_last_setup_signature = usb_setup_signature(request_type, request,
		value, index, length);
	runtime->usb_setup_captures++;
	runtime->usb_last_type = request_type;
	runtime->usb_last_request = request;
	runtime->usb_last_value = value;
	runtime->usb_last_index = index;
	runtime->usb_last_length = length;
	runtime->usb_setup_pending = 1u;
	/* FUN_5650 acknowledges the captured SETUP by writing 1 here. */
	USB_CMD_630 = 1u;
}

/* The post-IROM USB setup is separate from the endpoint-bank setup.  The
 * latter makes the device enumerate; these registers restore the endpoint
 * map and interrupt route used by the Imilab ILM after IVB.
 *
 * FUN_59dc probes the PHY before branching on !(USB_CTRL_168C & 2). The
 * accepted custom profile retains the loader's PHY setup; the optional
 * probe is separate from bank reprogramming. The loader already set bit 1,
 * so on a real boot bank programming takes the else branch and
 * only raises SYS_INT_ENABLE bit 2 -- the bank writes below are for bring-up
 * variants, not the normal path.  INDEPENDENT_USB_REPROGRAM_BANKS=1 forces
 * them so the difference can be A/B tested on hardware. */
static int __attribute__((unused)) mt7601u_usb_program_banks(void)
{
	uint32_t value = USB_CTRL_168C;
	int reprogrammed = 0;

	if ((value & 2u) == 0u) {
		USB_REG(0x098) = 0x8Bu;
		USB_REG(0x0A8) = 0x8Bu;
		USB_REG(0x0B8) = 0x8Bu;
		USB_REG(0x088) = 0x8Bu;
		USB_REG(0x0C8) = 0x8Bu;
		USB_REG(0x0E8) = 0x8Bu;
		USB_REG(0x108) = 0x8Bu;
		USB_REG(0x128) = 0x8Bu;
		USB_REG(0x628) = 0xFFu;
		USB_REG(0x660) = 0x39u;

		/* Stock post-IVB sequence: 0x70, not IROM's temporary 0x60. */
		USB_REG(0x688) = 0x10u;
		USB_REG(0x688) = 0x70u;
		USB_REG(0x688) = 0x00u;
		USB_REG(0x688) = 0x60u;
		USB_CTRL_168C = 2u;
		USB_REG(0x6A0) = 0x31u;
		USB_REG(0x6A0) = 0x32u;
		USB_REG(0x6A0) = 0x33u;
		USB_REG(0x6A0) = 0x34u;
		USB_REG(0x6A0) = 0x35u;
		USB_REG(0x6A0) = 0x21u;
		USB_REG(0x6A0) = 0x22u;
		USB_REG(0x6A0) = 0x23u;
		USB_REG(0x6A0) = 0x24u;
		USB_REG(0x6A0) = 0x25u;
		USB_REG(0x6A0) = 0x26u;
		USB_REG(0x6A0) = 0x27u;
		USB_REG(0x6A0) = 0x28u;
		/* FUN_59dc loops to 9, so the burst ends at 0x29, not 0x28. */
		USB_REG(0x6A0) = 0x29u;
		USB_CMD_630 = 0x3Fu;
		usb_phy_bank_select = 0u;
		USB_CTRL_168C &= ~1u;
		reprogrammed = 1;
	} else {
		SYS_INT_ENABLE |= 4u;
		USB_REG(0x660) = 0x39u;
		USB_REG(0x6A0) = 0x31u;
		USB_REG(0x6A0) = 0x32u;
		USB_REG(0x6A0) = 0x33u;
		USB_REG(0x6A0) = 0x34u;
		USB_REG(0x6A0) = 0x35u;
		USB_REG(0x6A0) = 0x21u;
		USB_REG(0x6A0) = 0x22u;
		USB_REG(0x6A0) = 0x23u;
		USB_REG(0x6A0) = 0x24u;
		USB_REG(0x6A0) = 0x25u;
		USB_REG(0x6A0) = 0x26u;
		USB_REG(0x6A0) = 0x27u;
		USB_REG(0x6A0) = 0x28u;
		USB_REG(0x6A0) = 0x29u;
		USB_CMD_630 = 0x3Fu;
		USB_REG(0x6A0) = 0u;
		usb_phy_bank_select = (uint8_t)(value & 1u);
	}
	/* The IROM had already routed interrupts here (SYS_INT_ROUTE = 5, then
	 * |= 0x100 at IROM 0x100168).  Overwriting the register drops those bits
	 * and wedges USB, so only the three PHY-derived bits are assigned. */
	SYS_INT_ROUTE = (SYS_INT_ROUTE & ~0x0E00u) | 2u |
		((uint32_t)usb_phy_bank_select << 9) |
		((uint32_t)usb_phy_route_bad << 10) |
		((uint32_t)usb_phy_inverted << 11);
	return reprogrammed;
}

static void __attribute__((unused)) mt7601u_usb_program_endpoints(void)
{
	/* FUN_5758 order, which is load-bearing: it selects the live endpoint
	 * bank and the interrupt route first, then types the endpoints, and
	 * only then acknowledges the whole thing through USB+0x630.  The
	 * previous version wrote 0x630 before the bank select, which hands the
	 * USB engine an "endpoints reprogrammed" for banks it has not seen the
	 * new type descriptors of yet. */
	USB_CTRL_168C |= 1u;
	SYS_INT_ROUTE |= 0x200u;

	/* Ghidra labels these as 0x00401038..0x00401128.  USB_REG already
	 * supplies the 0x00401000 base, so the offsets must stay relative. */
	USB_REG(0x038) = 0x8Cu;
	USB_REG(0x058) = 0x88u;
	USB_REG(0x078) = 0x84u;
	USB_REG(0x098) = 0x89u;
	USB_REG(0x0B8) = 0x88u;
	USB_REG(0x048) = 0x88u;
	USB_REG(0x068) = 0x84u;
	USB_REG(0x088) = 0x89u;
	USB_REG(0x0A8) = 0x89u;
	USB_REG(0x0C8) = 0x89u;
	USB_REG(0x0E8) = 0x89u;
	USB_REG(0x108) = 0x88u;
	USB_REG(0x128) = 0x89u;
	/* Last, as in stock: this is the command register's "reprogrammed" ack. */
	USB_CMD_630 = 0x20u;
}

static void __attribute__((unused)) mt7601u_usb_register_events(void)
{
	/* Stock FUN_448 installs six callback slots through FUN_3a90.  The
	 * callback table itself is not used by the polling runtime, but these are
	 * hardware event-enable masks, not merely scheduler bookkeeping: without
	 * them the USB/WLAN event sources can remain suppressed after IVB. */
	const uint32_t event_mask = (1u << 0) | (1u << 1) |
		(1u << 0x10) | (1u << 0x11) |
		(1u << 0x18) | (1u << 0x19);

	WLAN_EVENT_STATUS |= event_mask;
	WLAN_EVENT_ENABLE |= event_mask;
}

/* FUN_56ec is the stock USB reset/resume event handler. It is separate from
 * FUN_59dc's one-time PHY/bank setup and is dispatched from one latched
 * USB+0x630 snapshot in mt7601u_usb_dispatch_interrupt. */
static void __attribute__((unused)) mt7601u_usb_resume_reset(void)
{
	USB_REG(0x628) = 0xFFu;
	USB_REG(0x660) = 0x39u;
	USB_REG(0x688) = 0x10u;
	USB_REG(0x688) = 0x70u;
	USB_REG(0x688) = 0x00u;
	USB_REG(0x688) = 0x60u;
	USB_REG(0x6A0) = 0x31u;
	USB_REG(0x6A0) = 0x32u;
	USB_REG(0x6A0) = 0x33u;
	USB_REG(0x6A0) = 0x34u;
	USB_REG(0x6A0) = 0x35u;
	USB_REG(0x6A0) = 0x21u;
	USB_REG(0x6A0) = 0x22u;
	USB_REG(0x6A0) = 0x23u;
	USB_REG(0x6A0) = 0x24u;
	USB_REG(0x6A0) = 0x25u;
	USB_REG(0x6A0) = 0x26u;
	USB_REG(0x6A0) = 0x27u;
	USB_REG(0x6A0) = 0x28u;
	/* FUN_56ec uses the same nine-value loop as FUN_59dc. */
	USB_REG(0x6A0) = 0x29u;
	USB_CMD_630 = 0x3Fu;
}

/* FUN_5904 is the stock suspend path. The diagnostic call and scheduler flag
 * are omitted; dispatch still applies the USB/SYS side effects from the same
 * interrupt snapshot used for setup, reset, and endpoint-map events. */
static void __attribute__((unused)) mt7601u_usb_suspend(void)
{
	USB_REG(0x630) = 8u;
	if (usb_phy_inverted == 0u) {
		SYS_REG(0x258) |= 0x80000000u;
	} else {
		USB_CTRL_168C |= 0x40u;
		SYS_REG(0x258) &= ~0x80000000u;
	}
	USB_REG(0x69C) = 0u;
}

void mt7601u_usb_debug_dump(struct mt7601u_runtime *runtime, const char *tag)
{
	(void)tag;
	runtime->fault_usb_cmd630 = USB_CMD_630;
	runtime->fault_usb_fifo = USB_FIFO_CONTROL;
	runtime->fault_usb_ctrl = USB_CTRL_168C;
	runtime->fault_sys_route = SYS_INT_ROUTE;
	runtime->fault_wlan_evt = WLAN_EVENT_STATUS;

	mt7601u_log_printf("usb[%s] 630=%08x 008=%08x 68c=%08x 214=%08x 23c=%08x 9e0=%08x\n",
			   tag, runtime->fault_usb_cmd630, runtime->fault_usb_fifo,
			   runtime->fault_usb_ctrl, SYS_INT_ENABLE,
			   runtime->fault_sys_route, runtime->fault_wlan_evt);
	mt7601u_log_printf("usb[%s] set=%02x %02x idx=%04x len=%04x\n",
			   tag, runtime->usb_last_type, runtime->usb_last_request,
			   runtime->usb_last_index, runtime->usb_last_length);
	mt7601u_log_printf("usb[%s] req=%u rd=%u retry=%u err=%u evt=%u it=%u\n",
			   tag, runtime->usb_ep0_requests, runtime->usb_ep0_reads,
			   runtime->usb_ep0_retry, runtime->usb_ep0_errors,
			   runtime->usb_control_events,
			   runtime->service_iterations);
	mt7601u_log_printf("usb[%s] cap=%u fifo=%u addr=%08x tx=%u ctrl=%u evt=%08x\n",
			   tag, runtime->usb_setup_captures,
			   runtime->usb_ep0_fifo_writes,
			   runtime->usb_ep0_last_fifo_addr,
			   runtime->usb_ep0_last_tx_count,
			   runtime->usb_ep0_last_control,
			   runtime->usb_ep0_last_event_status);
	mt7601u_log_printf("host[%s] rd=%u wr=%u bad=%u op=%u width=%u stage=%u addr=%08x val=%08x\n",
			   tag, runtime->host_read_count,
			   runtime->host_write_count,
			   runtime->host_reject_count,
			   runtime->host_last_operation, runtime->host_last_width,
			   runtime->host_last_stage, runtime->host_last_address,
			   runtime->host_last_value);
	if (mt7601u_trap_code != 0u)
		mt7601u_log_printf("usb[%s] TRAP at %08x\n", tag, mt7601u_trap_cause);
}

/* FUN_4b30: the control FIFO is ready when USB 0x628 bit 0 is set, and the
 * bit is cleared by writing 1 back.  This is the only "a SETUP arrived"
 * signal the vendor uses before it reads the setup block. */
static int __attribute__((unused)) usb_control_ready(void)
{
	if ((USB_EVENT_STATUS & 1u) == 0u)
		return 0;
	USB_EVENT_STATUS = 1u;
	return 1;
}

void mt7601u_usb_init(struct mt7601u_runtime *runtime)
{
	usb_shutdown_irq_mask = 0u;
	usb_memory_base = 0u;
	/* FUN_5758 is required, not optional: it is what takes USB_CTRL_168C
	 * from 2 to 3, selecting the live endpoint bank.  The storm it used to
	 * cause came from reading back USB 0x630 in the service loop, which has
	 * been removed -- not from programming these registers. */
	if (mt7601u_usb_program_banks())
		mt7601u_usb_program_endpoints();
	mt7601u_usb_register_events();
	/* start_kernel releases the USB FIFO path after endpoint setup. */
	USB_REG_710 = 2u;
	/* The host has already selected the sole interface configuration while
	 * BootROM owns initial enumeration. A later bus reset clears this value. */
	runtime->usb_configuration = 1u;
	runtime->last_usb_status = USB_RX_COUNT;
	runtime->stage_mask |= MT7601U_STAGE_USB;
	mt7601u_usb_debug_dump(runtime, "init");
	mt7601u_publish_transport_diagnostics(runtime);
}

static void __attribute__((unused)) mt7601u_usb_service_transactions(
		struct mt7601u_runtime *runtime, uint32_t wlan_status)
{
	if (runtime->usb_setup_pending != 0u) {
		mt7601u_usb_ep0_service(runtime);
		runtime->usb_control_events++;
	}
	if (wlan_status != 0u)
		runtime->usb_bulk_events++;
	if ((runtime->stage_mask & MT7601U_STAGE_FCE) != 0u)
		mt7601u_fce_service(runtime);
	mt7601u_packet_service(runtime, wlan_status);
	mt7601u_publish_transport_diagnostics(runtime);
}

void mt7601u_usb_irq(struct mt7601u_runtime *runtime)
{
	uint32_t usb_status;
	uint32_t wlan_status;
	uint32_t usb_cmd;
	uint32_t saved_psw;

	/* Linux clears WLAN_EN before USB core resets/re-enumerates the device.
	 * Keep one owner of SETUP in that interval: foreground polling while
	 * WLAN is disabled, ISR capture once WLAN is enabled again. Preserve
	 * all other interrupt sources and restore only our previous USB bit. */
	if ((WLAN_REG(0x80u) & 1u) == 0u) {
		usb_shutdown_irq_mask |= SYS_INT_ENABLE & 4u;
		SYS_INT_ENABLE &= ~4u;
	} else if (usb_shutdown_irq_mask != 0u) {
		SYS_INT_ENABLE |= usb_shutdown_irq_mask;
		usb_shutdown_irq_mask = 0u;
	}
	/* Stock captures SETUP only in the USB ISR. Do not recapture an already
	 * acknowledged event from foreground command-register readback; defer
	 * EP0/FCE processing until after IRET has restored the task context. */
	if (runtime->usb_ep0_reads != 0u ||
		MMIO32(MT7601U_IRQ_DIAG_BASE + 8u) != 0u) {
		if ((WLAN_REG(0x80u) & 1u) != 0u) {
#ifdef __NDS32__
		MMIO32(MT7601U_IRQ_DIAG_BASE + 4u) =
			__nds32__mfsr(__NDS32_REG_PSW__);
#endif
		saved_psw = usb_ep0_save_and_disable_gie();
		/* The active-WLAN path remains ISR-owned. */
		MMIO32(MT7601U_IRQ_DIAG_BASE + 32u) = SYS_INT_ENABLE;
		runtime->service_iterations++;
		mt7601u_usb_service_transactions(runtime, WLAN_EVENT_STATUS);
		usb_ep0_restore_gie(saved_psw);
		return;
		}
	}
	saved_psw = usb_ep0_save_and_disable_gie();
	MMIO32(MT7601U_IRQ_DIAG_BASE + 4u) = saved_psw;

	/* Foreground polling and independent_interrupt_dispatch share the
	 * transaction service. Without the guard above, an ISR
	 * can land inside the poll loop's copy and both run the capture and the
	 * EP0 transaction against the same setup packet and the same
	 * write-1-to-ack registers -- which shows up host-side as a control
	 * transfer that answers with a truncated data stage (-EPROTO).  In the
	 * ISR GIE is already clear, so the guard is a no-op there and the
	 * restore hands control back with the PSW we arrived with. */
	usb_status = USB_EVENT_STATUS;
	wlan_status = WLAN_EVENT_STATUS;
	usb_cmd = USB_CMD_630;
	/* With CPU GIE masked, consume the latched SYS USB source through the
	 * same reset/map handlers as the IRQ entry. USB+0x630 alone also reads
	 * back command acknowledgements, so it is not a standalone event gate. */
	if ((SYS_INT_STATUS & 4u) != 0u && (usb_cmd & 0x38u) != 0u) {
		mt7601u_usb_dispatch_interrupt(runtime);
		mt7601u_usb_service_transactions(runtime, wlan_status);
		usb_ep0_restore_gie(saved_psw);
		return;
	}

	runtime->usb_irq_count++;
	runtime->last_usb_status = usb_status;
	runtime->last_usb_endpoint_status = usb_cmd;
	runtime->last_wlan_status = wlan_status;
	runtime->service_iterations++;

	/* FUN_597c is the vendor's USB ISR (registered for SYS source 2 by
	 * FUN_6a90(2, 0x597c)).  It reads USB 0x630 and dispatches:
	 *   bit 0 -> FUN_5650  capture SETUP
	 *   bit 3 -> FUN_5904  suspend
	 *   bit 4 -> FUN_56ec  resume/reset
	 *   bit 5 -> FUN_5758  reprogram endpoints
	 * So 0x630 is both written as a command and read back as an event
	 * bitmap, and bit 0 is the real "a SETUP is waiting" signal.
	 *
	 * The old poll loop used the whole bitmap, and bit 5 is what made it
	 * livelock: FUN_5758 writes 0x20 back into 0x630, so a poller that acts
	 * on bit 5 re-enters the endpoint programmer forever and never services
	 * anything. The acceptance profile qualifies bits 3/4/5 with the pending
	 * SYS USB source above; other diagnostic pollers inspect only setup. */
	if (runtime->usb_setup_pending == 0u) {
		/* Non-setup events need the pending SYS source (acceptance) or the
		 * real interrupt entry; command readback alone cannot qualify them. */
		uint8_t request_type = usb_setup_byte(0u);
		uint8_t request = usb_setup_byte(4u);
		uint16_t value = usb_setup_word(8u);
		uint16_t index = usb_setup_word(16u);
		uint16_t length = usb_setup_word(24u);
		uint32_t signature = usb_setup_signature(request_type, request,
			value, index, length);
		/* In the IRQ-masked profile standard SETUPs need the same changed
		 * packet fallback as vendor SETUPs. The hardware bit still handles
		 * repeated identical requests, as verified by live control tests. */
		int vendor_setup = 1;

		if (((usb_cmd & 1u) != 0u
			&& (SYS_INT_STATUS & 4u) != 0u
			) ||
		    (vendor_setup && signature != runtime->usb_last_setup_signature)) {
			mt7601u_usb_capture_setup(runtime, signature);
			runtime->service_progress++;
		}
	}

	mt7601u_usb_service_transactions(runtime, wlan_status);

	usb_ep0_restore_gie(saved_psw);
}

void mt7601u_usb_dispatch_interrupt(struct mt7601u_runtime *runtime)
{
	uint32_t saved_psw = usb_ep0_save_and_disable_gie();
	uint32_t event_status = USB_CMD_630;
	MMIO32(MT7601U_IRQ_DIAG_BASE + 36u) = event_status;

	runtime->usb_irq_count++;
	runtime->last_usb_endpoint_status = event_status;
	runtime->service_iterations++;

	/* Mirror FUN_597c's order and dispatch from one latched snapshot.  The
	 * foreground poller intentionally does not inspect bits 3/4/5: the stock
	 * handlers acknowledge those event commands by writing the same bits back,
	 * which would make a level poller repeat endpoint reprogramming forever. */
	/* A new SETUP aborts/replaces any unfinished control request. Stock
	 * FUN_597c always calls FUN_5650 for bit 0, even with pending service;
	 * suppressing capture here leaves a FIFO-not-ready retry stuck forever. */
	if ((event_status & 1u) != 0u) {
		mt7601u_usb_capture_setup(runtime, 0u);
		runtime->service_progress++;
	}
	if ((event_status & 8u) != 0u) {
		mt7601u_usb_suspend();
		mt7601u_wow_suspend();
		runtime->usb_suspend_events++;
	}
	if ((event_status & 0x10u) != 0u) {
		mt7601u_usb_resume_reset();
		mt7601u_wow_reset();
		runtime->usb_reset_events++;
		runtime->usb_configuration = 0u;
	}
	if ((event_status & 0x20u) != 0u) {
		mt7601u_usb_program_endpoints();
		runtime->usb_map_events++;
	}
	/* FUN_597c ends with a readback of USB+0x630 after the W1C event
	 * writes. Bound those writes/readback with DSB so acknowledgement finishes
	 * before IRET can expose the next pending event. */
#ifdef __NDS32__
	__nds32__dsb();
#endif
	(void)USB_CMD_630;
#ifdef __NDS32__
	__nds32__dsb();
#endif
	usb_ep0_restore_gie(saved_psw);
}

/* Called only by the assembly interrupt vector, never by foreground polls. */
uint32_t mt7601u_usb_record_interrupt(void)
{
	uint32_t pending = SYS_INT_STATUS;
	MMIO32(MT7601U_IRQ_DIAG_BASE + 8u)++;
	MMIO32(MT7601U_IRQ_DIAG_BASE + 16u) = pending;
	return pending;
}
