#include "mt7601u_mmio.h"
#include "mt7601u_runtime.h"
#include "mt7601u_log_ring.h"
#include "mt7601u_efuse.h"
#include "mt7601u_rf_bbp.h"
#ifdef __NDS32__
#include <nds32_intrinsic.h>
#endif

/* Service-loop pacing.
 *
 * A bare `for(;;) mt7601u_usb_irq()` reads three MMIO registers back to back
 * with nothing in between.  On this part that storm is itself a failure mode,
 * and it also left no way to tell a healthy idle device from a wedged one.
 * These bounds make the loop observable without turning a quiet bus into a
 * permanent stop: the loop keeps running, but a device that has been asked a
 * question and cannot answer it is reported and flagged. */
#define SERVICE_IDLE_BUDGET	2000000u
#define SERVICE_BACKOFF		64u

/* The station layout keeps runtime state in ILM. BootROM owns the DLM
 * workspace during firmware upload. */
struct mt7601u_runtime runtime = { .fault = 0xffffffffu };
static struct mt7601u_efuse_snapshot efuse_snapshot;

/* Written before the exception handler parks. */
volatile uint32_t mt7601u_trap_code;
volatile uint32_t mt7601u_trap_cause;

/* Stock's IRQ wrapper reloads the current task stack after the dispatcher.
 * Keep the same hand-off point even while this runtime is polling. It
 * makes the vector safe once the USB source is enabled by the hardware. */
volatile uint32_t independent_interrupt_sp;
extern void independent_enter_task(void);

extern uint32_t __bss_start;
extern uint32_t __bss_end;

static void clear_bss(void)
{
	volatile uint32_t *word;

	for (word = &__bss_start; word < &__bss_end; word++)
		*word = 0;
}

static void service_backoff(void)
{
	uint32_t n = SERVICE_BACKOFF;

	while (n-- != 0u)
		__asm__ volatile ("nop");
}

/* True once the host has asked EP0 something we failed to answer.  That is a
 * real stall; a device the driver simply has not probed yet is not. */
static int usb_is_stalled(const struct mt7601u_runtime *runtime)
{
	return runtime->usb_ep0_requests != 0u &&
		runtime->usb_ep0_reads == 0u &&
		runtime->usb_ep0_errors == 0u;
}

void independent_entry(void)
{
	uint32_t idle;
	uint32_t last_progress;
	uint32_t stall_reports = 0u;
	uint32_t reported_suspends = 0u;
	uint32_t reported_resets = 0u;

	clear_bss();
	/* These small-common symbols are linked after .bss on this toolchain,
	 * so clear them explicitly before the first USB diagnostic can inspect
	 * the trap flag. */
	mt7601u_trap_code = 0u;
	mt7601u_trap_cause = 0u;
	independent_interrupt_sp = 0u;
	for (idle = MT7601U_IRQ_DIAG_BASE; idle < MT7601U_IRQ_DIAG_END; idle += 4u)
		MMIO32(idle) = 0u;
	MMIO32(MT7601U_IRQ_DIAG_BASE) = MT7601U_IRQ_DIAG_MAGIC;
	mt7601u_log_ring_reset();
	runtime.stage_mask = 0;
	runtime.fault = 0;
	mt7601u_log_printf("independent: reset\n");
	mt7601u_boot_init(&runtime);
	mt7601u_log_event(MT_EVENT_BSP_READY, 0u, 0u);
	/* Before EP0/FCE service can advertise readiness, exclusively own eFuse.
	 * A failed snapshot is observable; transport still comes up for diagnosis. */
	(void)mt7601u_efuse_snapshot_init(&efuse_snapshot);
	mt7601u_efuse_log(&efuse_snapshot);
	/* Host pre-upload clock setup has completed. Linux does not yet own CSR access. */
	mt7601u_rf_bbp_boot_report();
	mt7601u_usb_init(&runtime);
	mt7601u_log_printf("independent: usb ok\n");
	SYS_RUN = 1u;
	mt7601u_fce_init(&runtime);
	/* The stock start_kernel path clears stale PSE fields after initializing
	 * the FCE rings. Keep this separate from the IROM USB re-arm sequence:
	 * custom EP0 capture showed 0x410804's high byte (0xF6) left RX DMA busy,
	 * while this Ghidra-backed mask produced the idle 0x00154f0f control state. */
	WLAN_REG(0x804) &= 0x00FFFFFFu;
	mt7601u_log_printf("independent: fce %08x\n", runtime.fault);
	mt7601u_dma_init(&runtime);
	mt7601u_log_printf("independent: stages %08x\n", runtime.stage_mask);
	/* This is the stock post-initialization boundary: vectors, USB banks,
	 * rings, and scheduler-visible state are ready before USB IRQs can fire. */
	mt7601u_log_printf("independent: ready %08x\n", runtime.stage_mask);
	/* Stock reset runs at PSW=0xA (INTL=1), but start_kernel enters a
	 * synthetic task frame via IRET: FUN_6654 initializes IPSW=0x8 and
	 * FUN_6558 enables GIE at task level. Our direct C entry needs the same
	 * exception return to INTL=0 before unmasking. */
	independent_enter_task();
	__builtin_nds32_setgie_en();
	__nds32__dsb();

	idle = 0u;
	last_progress = runtime.service_progress;
	for (;;) {
		uint32_t suspends = runtime.usb_suspend_events;
		uint32_t resets = runtime.usb_reset_events;

		/* The IRQ handler only increments the counters. The foreground
		 * writer serializes the ring and reports each observed batch once. */
		if (suspends != reported_suspends) {
			mt7601u_log_event(MT_EVENT_USB_SUSPEND, suspends - reported_suspends, 0u);
			reported_suspends = suspends;
		}
		if (resets != reported_resets) {
			mt7601u_log_event(MT_EVENT_USB_RESUME, resets - reported_resets, 0u);
			reported_resets = resets;
		}
		mt7601u_usb_irq(&runtime);
		/* Allow the USB engine/interrupt route to settle between masked
		 * foreground transactions. Pacing only the diagnostic idle-budget
		 * boundary leaves millions of consecutive MMIO-heavy iterations. */
		service_backoff();
		if (runtime.service_progress != last_progress) {
			last_progress = runtime.service_progress;
			idle = 0u;
			continue;
		}
		idle++;
		if (idle < SERVICE_IDLE_BUDGET)
			continue;
		idle = 0u;

		if (usb_is_stalled(&runtime)) {
			/* The host asked and we never produced an answer.  Report
			 * it a bounded number of times, then stay in the loop so a
			 * later recovery is still possible. */
			if (stall_reports < 4u) {
				stall_reports++;
				runtime.fault = MT7601U_FAULT_STALLED;
				mt7601u_log_printf("independent: stalled req=%u rd=%u\n",
						   runtime.usb_ep0_requests,
						   runtime.usb_ep0_reads);
				mt7601u_usb_debug_dump(&runtime, "stall");
			}
		}
		service_backoff();
	}
}

/* Called by the NDS32 USB interrupt vector.  Keep the interrupt-facing
 * wrapper small while the rest of the hardware interrupt sources are still
 * being reconstructed; the USB dispatcher itself acknowledges its source
 * registers; foreground services EP0/FCE state. */
void independent_interrupt_dispatch(void)
{
	uint32_t pending = mt7601u_usb_record_interrupt();
	/* Like stock's source dispatcher, qualify USB events with SYS source 2.
	 * USB+0x630 also reads back acknowledgements after the source clears. */
	if ((pending & 4u) != 0u)
		mt7601u_usb_dispatch_interrupt(&runtime);
}
