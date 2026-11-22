/* Prototype compact station layout (contiguous ILM image).
 *
 * Only the vectors (0x0), entry (0xa4), the log ring (0xd000), the host-read
 * IQ trace (0xc000) and packet diagnostics (0xc900) are fixed.  Everything
 * else is packed contiguously; nothing in the image is placed by object name,
 * so the layout survives LTO.  Zero-initialised runtime state is NOLOAD and
 * cleared by clear_bss() in main.c.
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
		*(.text .text.*)
		*(.portguard)
		*(.rodata .rodata.*)
		*(.caldiag)
		*(.events)
	} > ILM

	/* Keep descriptor and payload bases at least doubleword-aligned. */
	.data ALIGN(8) : {
		*(.data .data.*)
		*(.sdata .sdata.*)
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

	/* Runtime state follows the uploaded image but is not uploaded. */
	.bss (NOLOAD) : ALIGN(8) {
		__bss_start = .;
		*(.sbss .sbss.* .scommon_w .scommon_h .scommon_b .scommon)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > ILM

	/* Host-readable (EP0 memory read) trace and packet evidence keep their
	 * addresses.  Both are zeroed at boot instead of uploaded. */
	.iqtrace 0x0000c000 (NOLOAD) : { . += 0x804; } > ILM
	.packetdiagnostics 0x0000c900 (NOLOAD) : { . += 0xc0; } > ILM

	ASSERT((runtime & 7) == 0, "runtime DMA storage is not doubleword-aligned")
	ASSERT(__ilm_end <= 0xc000, "firmware image overlaps the IQ trace")
	ASSERT(__bss_end <= 0xc000, "runtime state overlaps the IQ trace")
	ASSERT(__ilm_end <= 0xd000, "firmware overlaps the log ring")
	__dlm_end = .;
	/* Stay below the BootROM-owned descriptor/IRQ region at 0xfc00. */
	_stack_top = 0x0000fb00;
	ASSERT(_stack_top < 0x0000fc00, "stack overlaps BootROM scratch")

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
