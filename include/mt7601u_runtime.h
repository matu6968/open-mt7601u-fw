#ifndef MT7601U_RUNTIME_H
#define MT7601U_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#include "mt7601u_hw.h"
#include "mt7601u_evidence.h"

enum mt7601u_runtime_stage {
	MT7601U_STAGE_BOOT = 1u << 0,
	MT7601U_STAGE_USB = 1u << 1,
	MT7601U_STAGE_FCE = 1u << 2,
	MT7601U_STAGE_DMA = 1u << 3,
	/* Set once EP0 has completed a real vendor read, i.e. the host can be
	 * served and COM_REG0 may be advertised. */
	MT7601U_STAGE_ALIVE = 1u << 8,
};

enum mt7601u_fault {
	MT7601U_FAULT_NONE	= 0u,
	/* A trap was taken before or during bring-up; see mt7601u_trap_code. */
	MT7601U_FAULT_TRAP	= 1u,
	/* The bounded service loop ran out of iterations with no progress. */
	MT7601U_FAULT_STALLED	= 2u,
};

struct mt7601u_runtime {
	uint32_t stage_mask;
	uint32_t fault;
	uint32_t usb_irq_count;
	uint32_t usb_control_events;
	uint32_t usb_bulk_events;
	uint32_t last_usb_status;
	uint32_t last_usb_endpoint_status;
	uint32_t last_wlan_status;
	uint32_t usb_standard_requests;
	uint32_t usb_standard_errors;
	volatile uint32_t usb_reset_events;
	uint32_t usb_map_events;
	volatile uint32_t usb_suspend_events;
	uint32_t usb_ep0_requests;
	uint32_t usb_ep0_errors;
	uint32_t usb_setup_captures;
	uint32_t usb_ep0_fifo_writes;
	uint32_t usb_ep0_arms;
	uint32_t usb_ep0_last_fifo_addr;
	uint32_t usb_ep0_last_event_status;
	uint32_t usb_ep0_last_tx_count;
	uint32_t usb_ep0_last_control;
	uint32_t usb_last_setup_signature;
	volatile uint8_t usb_setup_pending;
	volatile uint8_t usb_last_request;
	volatile uint8_t usb_last_type;
	uint8_t usb_configuration;
	volatile uint16_t usb_last_value;
	volatile uint16_t usb_last_index;
	volatile uint16_t usb_last_length;
	/* EP0 progress markers.  usb_ep0_retry counts the not-ready path that
	 * re-arms instead of stalling; usb_ep0_reads counts completed reads so
	 * the runtime can advertise liveness as soon as the host can be served. */
	uint32_t usb_ep0_reads;
	uint32_t usb_ep0_retry;
	uint32_t usb_ep0_writes;
	/* Service-loop liveness, read by the bounded loop in main.c. */
	uint32_t service_iterations;
	uint32_t service_progress;
	/* Snapshot of the registers that decide whether USB can progress.  Taken
	 * when a fault is raised so the state survives on the board. */
	uint32_t fault_usb_cmd630;
	uint32_t fault_usb_fifo;
	uint32_t fault_usb_ctrl;
	uint32_t fault_sys_route;
	uint32_t fault_wlan_evt;
	/* Host-visible register backend diagnostics.  EP0 offsets are recorded in
	 * MCU WLAN address form so the same address identifies EP0 and FCE paths. */
	uint32_t host_read_count;
	uint32_t host_write_count;
	uint32_t host_reject_count;
	uint32_t host_last_address;
	uint32_t host_last_value;
	uint8_t host_last_operation;
	uint8_t host_last_width;
	uint8_t host_last_stage;
	struct mt7601u_fce_state fce;
};

/* Phase 8 command parity (stock-matching unknown-command silence, read-side
 * commands, FUN_SET/BURST_WRITE length rules). Off for every earlier image so
 * their bytes stay reproducible; the independent-station-p8 target enables it. */

/* The station linker aligns runtime itself. Verify the embedded ring and
 * payload offsets with the target ABI, rather than a host pointer-size guess. */
#define MT7601U_CHECK_DMA_OFFSET(member) \
	_Static_assert((offsetof(struct mt7601u_runtime, fce.member) & 7u) == 0u, \
		"FCE storage must preserve doubleword alignment: " #member)
MT7601U_CHECK_DMA_OFFSET(tx0);
MT7601U_CHECK_DMA_OFFSET(tx1);
MT7601U_CHECK_DMA_OFFSET(cpu_cmd);
MT7601U_CHECK_DMA_OFFSET(cmd_resp);
MT7601U_CHECK_DMA_OFFSET(tx0_payload);
MT7601U_CHECK_DMA_OFFSET(tx1_payload);
MT7601U_CHECK_DMA_OFFSET(response_payload);
#undef MT7601U_CHECK_DMA_OFFSET

enum mt7601u_host_operation {
	MT7601U_HOST_OP_NONE = 0u,
	MT7601U_HOST_OP_EP0_READ = 1u,
	MT7601U_HOST_OP_EP0_WRITE = 2u,
	MT7601U_HOST_OP_FCE_WRITE = 3u,
	MT7601U_HOST_OP_UNSUPPORTED = 15u,
};

enum mt7601u_host_stage {
	MT7601U_HOST_STAGE_REQUEST = 1u,
	MT7601U_HOST_STAGE_VALIDATED = 2u,
	MT7601U_HOST_STAGE_WAIT_READY = 3u,
	MT7601U_HOST_STAGE_MMIO = 4u,
	MT7601U_HOST_STAGE_COMPLETE = 5u,
	MT7601U_HOST_STAGE_FIFO_LOADED = 6u,
	MT7601U_HOST_STAGE_ARMED = 7u,
	MT7601U_HOST_STAGE_REJECTED = 15u,
};

/* COM_REG2 tags the most recent operation, stage, and offset. COM_REG3
 * carries the redacted diagnostic value. A read of either mailbox leaves
 * their contents intact. */
static inline void mt7601u_publish_transport_diagnostics(
		const struct mt7601u_runtime *runtime)
{
	if (runtime->host_last_address == MT_MCU_WLAN_BASE + MT_HOST_COM_REG2 ||
		runtime->host_last_address == MT_MCU_WLAN_BASE + MT_HOST_COM_REG3)
		return;
	WLAN_COM_REG2 = 0xD6000000u |
		((uint32_t)(runtime->host_last_stage & 0x0fu) << 20) |
		((uint32_t)(runtime->host_last_operation & 0x0fu) << 16) |
		(runtime->host_last_address & 0xffffu);
	WLAN_COM_REG3 = runtime->host_last_value;
}

void mt7601u_boot_init(struct mt7601u_runtime *runtime);
void mt7601u_usb_init(struct mt7601u_runtime *runtime);
void mt7601u_fce_init(struct mt7601u_runtime *runtime);
void mt7601u_dma_init(struct mt7601u_runtime *runtime);
void mt7601u_usb_irq(struct mt7601u_runtime *runtime);
void mt7601u_usb_dispatch_interrupt(struct mt7601u_runtime *runtime);
uint32_t mt7601u_usb_record_interrupt(void);
void mt7601u_usb_ep0_service(struct mt7601u_runtime *runtime);
void mt7601u_fce_service(struct mt7601u_runtime *runtime);

/* Host register access backend.  Call begin once per host operation, then
 * use a width-specific accessor. Unsupported address/width combinations are
 * recorded and rejected rather than answered with synthetic data. */
void mt7601u_host_access_begin(struct mt7601u_runtime *runtime,
		uint8_t operation, uint32_t address, uint8_t width,
		uint32_t value);
void mt7601u_host_access_stage(struct mt7601u_runtime *runtime,
		uint8_t stage, uint32_t value);
void mt7601u_host_access_reject(struct mt7601u_runtime *runtime);
int mt7601u_host_read32(struct mt7601u_runtime *runtime,
		uint32_t address, uint32_t *value);
int mt7601u_host_write16(struct mt7601u_runtime *runtime,
		uint32_t address, uint16_t value);
int mt7601u_host_write32(struct mt7601u_runtime *runtime,
		uint32_t address, uint32_t value);
int mt7601u_host_address_valid(uint32_t address, uint8_t width);

/* COM_REG0/1 runtime contract. COM_REG1 bit 31 during host FCE upload is
 * hardware completion; this function publishes only the image identity. */
int mt7601u_mailbox_publish_running(struct mt7601u_runtime *runtime);
int mt7601u_led_mode_valid(uint32_t channel, uint32_t mode);
void mt7601u_led_set_mode(uint32_t channel, uint32_t mode);

/* Firmware diagnostics.  This is deliberately independent of USB/FCE so it
 * can be used while those paths are being brought up. */
void mt7601u_log_ring_reset(void);
#if INDEPENDENT_LOGGING
void mt7601u_log_printf(const char *format, ...);
#else
#define mt7601u_log_printf(...) ((void)0)
#endif

/* Snapshot EP0 state into the fault fields and verbose log ring. */
void mt7601u_usb_debug_dump(struct mt7601u_runtime *runtime, const char *tag);

/* Written by independent_exception in entry.S before it parks, so a trap
 * during bring-up is reported instead of spinning silently.  mt7601u_trap_code
 * is the flag; mt7601u_trap_cause is IPC, the address execution was at. */
extern volatile uint32_t mt7601u_trap_code;
extern volatile uint32_t mt7601u_trap_cause;

#endif
