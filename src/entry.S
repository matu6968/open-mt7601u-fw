/* Vendor-free runtime entry. The CSR setup mirrors the recovered Imilab
 * reset contract; device-specific USB/FCE ownership is still staged in C.
 */
	.section .text.start, "ax"
	.p2align 2
	.globl _start
	.type _start, @function
_start:
	/* The C runtime is compiled with $gp-relative small-data accesses
	 * (swi.gp/lwi.gp).  Neither BootROM nor the stock reset path ever
	 * programs $gp, so without this every small-data store lands at
	 * 0xFFFFxxxx instead of the runtime variables. */
	la	$gp, _SDA_BASE_
	/* Keep the IVB control transfer's completion on the USB wire before the
	 * image changes PSW, IVB, or interrupt state.  The hello-only diagnostic
	 * already uses this pair of bounded loops at the handoff boundary. */
	movi	$r2, 0x3fff
5:
	addi	$r2, $r2, -1
	bnez	$r2, 5b
	movi	$r2, 0x3fff
6:
	addi	$r2, $r2, -1
	bnez	$r2, 6b
	/* Keep GIE masked until independent_entry has completed the stock USB
	 * preamble and installed all DLM/runtime state. */
	movi	$r0, 0xA
	mtsr	$r0, $psw
	/* Stock reset masks SYS sources at 0x00400204 before installing the
	 * new vector table.  The later CSR mask is separate and still uses
	 * 0xC0000002 in the self-contained image. */
	sethi	$r0, 0x400
	ori	$r0, $r0, 0x204
	movi	$r1, 0x401
	swi	$r1, [$r0]
	movi	$r0, 0
	mtsr	$r0, $ivb
	sethi	$r0, 0xc0000
	ori	$r0, $r0, 2
	mtsr	$r0, $int_mask
	sethi	$sp, 0x0f
	ori	$sp, $sp, 0xb00
	jal	independent_entry
1:
	j	1b
	.size _start, .-_start

	/* Common vector target: recognize the observed USB interrupt cause
	 * after saving the entire context; other causes retain the diagnostic park. */
	.globl independent_exception
	.type independent_exception, @function
independent_exception:
	smw.adm $r0, [$sp], $r10, 0xe
	smw.adm $r15, [$sp], $r15, 0
	mfsr $r0, $itype
	andi $r0, $r0, 0xf
	movi $r1, 3
	bne $r0, $r1, independent_irq_fault
	movi $r0, 0xe05c
	movi $r1, 0x40
	swi $r1, [$r0]
	j independent_irq_saved
independent_irq_fault:
	mfsr $r1, $ipc
	/* Store the actual saved PC before parking, including a zero PC. */
	la	$r0, mt7601u_trap_cause
	swi	$r1, [$r0]
	la	$r0, mt7601u_trap_code
	movi	$r1, 1
	swi	$r1, [$r0]
1:
	j	1b
	.size independent_exception, .-independent_exception

	/* Stock USB IRQ entry at IVB target 0x48 saves the interrupted context,
	 * calls the registered dispatcher, restores IPSW/IPC/PSW, and returns via
	 * IRET.  The short vector stub in independent_vectors.S jumps here so the
	 * complete frame does not consume the fixed 0x40..0xA3 vector area. */
	.p2align 2
	.globl independent_irq_entry
	.type independent_irq_entry, @function
independent_irq_entry:
	smw.adm	$r0, [$sp], $r10, 0xe
	smw.adm	$r15, [$sp], $r15, 0x0
	movi $r0, 0xe05c
	movi $r1, 0x48
	swi $r1, [$r0]
independent_irq_saved:
	mfsr	$r6, $ipsw
	mfsr	$r7, $ipc
	mfsr	$r8, $psw
	smw.adm	$r6, [$sp], $r8, 0x0
	movi $r0, 0xe02c
	swi $r6, [$r0]
	movi $r0, 0xe050
	swi $r7, [$r0]
	movi $r0, 0xe058
	swi $r8, [$r0]
	/* Stock stores the post-frame SP in a scheduler-owned DLM word and
	 * reloads it after the dispatcher.  Preserve that ABI hand-off here. */
	la	$r0, independent_interrupt_sp
	swi	$sp, [$r0]
	jal	independent_interrupt_dispatch
	la	$r0, independent_interrupt_sp
	lwi	$sp, [$r0]
	lmw.bim	$r6, [$sp], $r8, 0x0
	mtsr	$r6, $ipsw
	mtsr	$r7, $ipc
	mtsr	$r8, $psw
	lmw.bim	$r15, [$sp], $r15, 0x0
	lmw.bim	$r0, [$sp], $r10, 0xe
	iret
	.size independent_irq_entry, .-independent_irq_entry

	/* Stock start_kernel enters its first task through IRET, with IPSW=8
	 * and an interrupt-level PSW=0xA. Do the same exception return rather
	 * than editing INTL in the live PSW while the reset context is active. */
	.globl independent_enter_task
	.type independent_enter_task, @function
independent_enter_task:
	movi $r0, 8
	mtsr $r0, $ipsw
	la $r0, 8f
	mtsr $r0, $ipc
	dsb
	iret
8:
	ret $lp
	.size independent_enter_task, .-independent_enter_task
