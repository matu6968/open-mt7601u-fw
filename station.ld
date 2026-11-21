/* Isolated stage 7 station layout.
 *
 * Every stock MT7601U image has DLM length zero.  Loading custom data at
 * 0x78000 can overwrite BootROM workspace before firmware takes ownership,
 * so this profile keeps mutable runtime below the preserved SRAM at
 * 0xfc00 and emits no DLM payload.
 */
OUTPUT_FORMAT("elf32-nds32le")
OUTPUT_ARCH(nds32)
ENTRY(_start)

MEMORY
{
	ILM (rwx) : ORIGIN = 0x00000000, LENGTH = 0x10000
}

SECTIONS
{
	.ivb 0x00000000 : {
		KEEP(*(.ivb))
	} > ILM

	.pad40 0x00000040 : {
		KEEP(*(.pad40))
	} > ILM

	.text 0x000000A4 : {
		KEEP(*(.text.start))
		KEEP(*(.text.vectors))
		*(EXCLUDE_FILE (*calibration.o *iq_calibration.o *bw_dpd.o *packet.o *wow.o *led.o) .text .text.*)
		*(EXCLUDE_FILE (*calibration.o *iq_calibration.o *bw_dpd.o *packet.o *wow.o *led.o) .rodata .rodata.*)
	} > ILM

	/* Keep descriptor and payload bases at least doubleword-aligned. The
	 * accepted packet image has this alignment; growth in USB text must not
	 * silently move the embedded rings to a four-byte-only address. */
	.data ALIGN(8) : {
		*(.data .data.*)
		*(.sdata .sdata.*)
	} > ILM
	ASSERT((runtime & 7) == 0, "runtime DMA storage is not doubleword-aligned")

	.bss (NOLOAD) : ALIGN(4) {
		__bss_start = .;
		*(EXCLUDE_FILE (*packet.o) .bss .bss.*)
		*(COMMON)
		__bss_end = .;
	} > ILM
	/* Keep accepted calibration globals and the GP base at their stage 5
	 * addresses. Packet state is independently initialized before readiness. */
	.packetstate 0x00006700 (NOLOAD) : {
		*packet.o(.bss .bss.* .scommon_w .scommon_h .scommon_b)
	} > ILM
	ASSERT(ADDR(.scommon_w) + SIZEOF(.scommon_w) <= 0x6700, "core globals overlap packet state")
	ASSERT(ADDR(.packetstate) + SIZEOF(.packetstate) <= 0x6c00, "packet state overlaps code")

	/* Calibration controller ownership preserves the host's real pending
	 * halfword pair or completion without moving accepted core globals. */
	.portguard 0x00006800 : { *(.portguard) } > ILM
	ASSERT(ADDR(.packetstate) + SIZEOF(.packetstate) <= 0x6800,
		"packet state overlaps controller guard")
	ASSERT(ADDR(.portguard) + SIZEOF(.portguard) <= 0x6c00,
		"controller guard overlaps packet code")

	/* Isolate new calibration instructions from the proven transport/core
	 * placement. Space below 0x7800 remains available to runtime state. */
	.packet 0x00006c00 : {
		*packet.o(.text .text.* .rodata .rodata.*)
	} > ILM
	.led ALIGN(4) : { *led.o(.text .text.* .rodata .rodata.*) } > ILM
	ASSERT(__bss_end <= 0x6c00, "core state overlaps packet code")
	ASSERT(ADDR(.scommon_w) + SIZEOF(.scommon_w) <= 0x6c00, "small common overlaps packet code")
	ASSERT(ADDR(.led) + SIZEOF(.led) <= 0x7800, "packet or LED code overlaps calibration")
	.calibration 0x00007800 : {
		*calibration.o(.text .text.* .rodata .rodata.*)
		*iq_calibration.o(.text .text.* .rodata .rodata.*)
		*bw_dpd.o(.text .text.* .rodata .rodata.*)
		*(.caldiag)
	} > ILM
	ASSERT(__bss_end <= 0x7800, "core state overlaps calibration code")
	ASSERT(ADDR(.scommon_w) + SIZEOF(.scommon_w) <= 0x7800,
		"small-common overlaps calibration code")
	ASSERT(ADDR(.calibration) + SIZEOF(.calibration) <= 0xc000,
		"calibration code overlaps IQ trace")
	/* Bounded diagnostic measurement trace: count plus 512 words. */
	.iqtrace 0x0000c000 : { LONG(0); . += 0x800; } > ILM
	/* Packet snapshots write these words at runtime. Reserve them even
	 * though they are not uploaded, so diagnostic code cannot occupy them. */
	.packetdiagnostics 0x0000c900 (NOLOAD) : { . += 0xc0; } > ILM
	.events 0x0000c9c0 : { *(.events) } > ILM
	.wowlan ALIGN(4) : {
		*wow.o(.text .text.* .rodata .rodata.*)
	} > ILM
	.image_footer ALIGN(4) : {
		KEEP(*(.image_usb.language))
		KEEP(*(.image_usb.manufacturer))
		KEEP(*(.image_usb.product))
		KEEP(*(.image_usb.serial))
		. = ALIGN(4);
		BYTE(0x56); BYTE(0x65); BYTE(0x72); BYTE(0x73); /* Vers */
		LONG(0xb1); /* Constant observed in vendor version records. */
		SHORT(0x0001); /* Open firmware build. */
		SHORT(0x0001); /* Open firmware version. */
	} > ILM
	__ilm_end = ADDR(.image_footer) + SIZEOF(.image_footer);
	ASSERT(__ilm_end <= 0xd000, "firmware overlaps the log ring")
	__dlm_end = .;
	/* Stay below the BootROM-owned descriptor/IRQ region at 0xfc00. */
	_stack_top = 0x0000fb00;
	ASSERT(__bss_end <= 0x0000d000, "runtime overlaps the ILM log ring")
	ASSERT(ADDR(.scommon_w) + SIZEOF(.scommon_w) <= 0x0000d000,
		"small-common runtime overlaps the ILM log ring")
	ASSERT(_stack_top < 0x0000fc00, "stack overlaps BootROM scratch")

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
