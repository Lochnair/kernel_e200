/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1994, 95, 96, 99, 2001 Ralf Baechle
 * Copyright (C) 1994, 1995, 1996 Paul M. Antoine.
 * Copyright (C) 1999 Silicon Graphics, Inc.
 * Copyright (C) 2007  Maciej W. Rozycki
 */
#ifndef _ASM_STACKFRAME_H
#define _ASM_STACKFRAME_H

#include <linux/threads.h>

#include <asm/asm.h>
#include <asm/asmmacro.h>
#include <asm/mipsregs.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/dwarf2.h>

/* Make the addition of cfi info a little easier. */
	.macro cfi_rel_offset reg offset=0 docfi=0
	.if \docfi
	CFI_REL_OFFSET \reg, \offset
	.endif
	.endm

	.macro cfi_st reg offset=0 docfi=0
	LONG_S	\reg, \offset(sp)
	cfi_rel_offset \reg, \offset, \docfi
	.endm

	.macro cfi_restore reg offset=0 docfi=0
	.if \docfi
	CFI_RESTORE \reg
	.endif
	.endm

	.macro cfi_ld reg offset=0 docfi=0
	LONG_L	\reg, \offset(sp)
	cfi_restore \reg \offset \docfi
	.endm

/*
 * For SMTC kernel, global IE should be left set, and interrupts
 * controlled exclusively via IXMT.
 */
#ifdef CONFIG_MIPS_MT_SMTC
#define STATMASK 0x1e
#elif defined(CONFIG_CPU_R3000) || defined(CONFIG_CPU_TX39XX)
#define STATMASK 0x3f
#else
#define STATMASK 0x1f
#endif

#ifdef CONFIG_MIPS_MT_SMTC
#include <asm/mipsmtregs.h>
#endif /* CONFIG_MIPS_MT_SMTC */

		.macro	SAVE_AT docfi=0
		.set	push
		.set	noat
		cfi_st	$1, PT_R1, \docfi
		.set	pop
		.endm

		.macro	SAVE_TEMP docfi=0
#ifdef CONFIG_CPU_HAS_SMARTMIPS
		mflhxu	v1
		LONG_S	v1, PT_LO(sp)
		mflhxu	v1
		LONG_S	v1, PT_HI(sp)
		mflhxu	v1
		LONG_S	v1, PT_ACX(sp)
#else
		mfhi	v1
#endif
#ifdef CONFIG_32BIT
		cfi_st	$8, PT_R8, \docfi
		cfi_st	$9, PT_R9, \docfi
#endif
		cfi_st	$10, PT_R10, \docfi
		cfi_st	$11, PT_R11, \docfi
		cfi_st	$12, PT_R12, \docfi
#ifndef CONFIG_CPU_HAS_SMARTMIPS
		LONG_S	v1, PT_HI(sp)
		mflo	v1
#endif
		cfi_st	$13, PT_R13, \docfi
		cfi_st	$14, PT_R14, \docfi
		cfi_st	$15, PT_R15, \docfi
		cfi_st	$24, PT_R24, \docfi
#ifndef CONFIG_CPU_HAS_SMARTMIPS
		LONG_S	v1, PT_LO(sp)
#endif
#ifdef CONFIG_CPU_CAVIUM_OCTEON
		/*
		 * The Octeon multiplier state is affected by general
		 * multiply instructions. It must be saved before and
		 * kernel code might corrupt it
		 */
		jal     octeon_mult_save
#endif
		.endm

		.macro	SAVE_STATIC docfi=0
		cfi_st	$16, PT_R16, \docfi
		cfi_st	$17, PT_R17, \docfi
		cfi_st	$18, PT_R18, \docfi
		cfi_st	$19, PT_R19, \docfi
		cfi_st	$20, PT_R20, \docfi
		cfi_st	$21, PT_R21, \docfi
		cfi_st	$22, PT_R22, \docfi
		cfi_st	$23, PT_R23, \docfi
		cfi_st	$30, PT_R30, \docfi
		.endm

#ifdef CONFIG_SMP
#ifdef CONFIG_MIPS_MT_SMTC
#define PTEBASE_SHIFT	19	/* TCBIND */
#define CPU_ID_REG CP0_TCBIND
#define CPU_ID_MFC0 mfc0
#elif defined(CONFIG_MIPS_PGD_C0_CONTEXT)
#define PTEBASE_SHIFT	48	/* XCONTEXT */
#define CPU_ID_REG CP0_XCONTEXT
#define CPU_ID_MFC0 MFC0
#else
#define PTEBASE_SHIFT	23	/* CONTEXT */
#define CPU_ID_REG CP0_CONTEXT
#define CPU_ID_MFC0 MFC0
#endif
#define CPU_ID_MASK ((1 << 13) - 1)

		.macro	get_saved_sp_for_save_some	/* SMP variation */
		CPU_ID_MFC0	k0, CPU_ID_REG
#if defined(CONFIG_32BIT) || defined(KBUILD_64BIT_SYM32)
		lui	k1, %hi(kernelsp)
#else
		lui	k1, %highest(kernelsp)
		daddiu	k1, %higher(kernelsp)
		dsll	k1, 16
		daddiu	k1, %hi(kernelsp)
		dsll	k1, 16
#endif
		LONG_SRL	k0, PTEBASE_SHIFT
		LONG_ADDU	k1, k0
		LONG_L	k1, %lo(kernelsp)(k1)
		.endm

		.macro get_saved_sp
		CPU_ID_MFC0	k0, CPU_ID_REG
		get_saved_sp_for_save_some
		.endm

		.macro	set_saved_sp stackp temp temp2
		CPU_ID_MFC0	\temp, CPU_ID_REG
		LONG_SRL	\temp, PTEBASE_SHIFT
		LONG_S	\stackp, kernelsp(\temp)
		.endm
#else
		.macro	get_saved_sp	/* Uniprocessor variation */
#ifdef CONFIG_CPU_JUMP_WORKAROUNDS
		/*
		 * Clear BTB (branch target buffer), forbid RAS (return address
		 * stack) to workaround the Out-of-order Issue in Loongson2F
		 * via its diagnostic register.
		 */
		move	k0, ra
		jal	1f
		 nop
1:		jal	1f
		 nop
1:		jal	1f
		 nop
1:		jal	1f
		 nop
1:		move	ra, k0
		li	k0, 3
		mtc0	k0, $22
#endif /* CONFIG_CPU_JUMP_WORKAROUNDS */
#if defined(CONFIG_32BIT) || defined(KBUILD_64BIT_SYM32)
		lui	k1, %hi(kernelsp)
#else
		lui	k1, %highest(kernelsp)
		daddiu	k1, %higher(kernelsp)
		dsll	k1, k1, 16
		daddiu	k1, %hi(kernelsp)
		dsll	k1, k1, 16
#endif
		LONG_L	k1, %lo(kernelsp)(k1)
		.endm
#if defined(CONFIG_MIPS_PGD_C0_CONTEXT)
#define PTEBASE_SHIFT	48	/* XCONTEXT */
#define CPU_ID_REG CP0_XCONTEXT
#define CPU_ID_MFC0 MFC0
#endif
		.macro	get_saved_sp_for_save_some	/* SMP variation */
#if defined(CONFIG_32BIT) || defined(KBUILD_64BIT_SYM32)
		lui	k1, %hi(kernelsp)
#else
		lui	k1, %highest(kernelsp)
		daddiu	k1, %higher(kernelsp)
		dsll	k1, 16
		daddiu	k1, %hi(kernelsp)
		dsll	k1, 16
#endif
		LONG_L	k1, %lo(kernelsp)(k1)
		.endm

		.macro	set_saved_sp stackp temp temp2
		LONG_S	\stackp, kernelsp
		.endm
#endif

		.macro	SAVE_SOME docfi=0
		.set	push
		.set	noat
		.set	reorder
		mfc0	k0, CP0_STATUS
		sll	k0, 3		/* extract cu0 bit */
		.set	noreorder
		bltz	k0, 8f
		 move	k1, sp
		.set	reorder
		/* Called from user mode, new stack. */
7:		get_saved_sp_for_save_some
#ifndef CONFIG_CPU_DADDI_WORKAROUNDS
8:		move	k0, sp
		.if \docfi
		CFI_REGISTER sp, k0
		.endif
		PTR_SUBU sp, k1, PT_SIZE
#else
		.set	at=k0
8:		PTR_SUBU k1, PT_SIZE
		.set	noat
		move	k0, sp
		.if \docfi
		CFI_REGISTER sp, k0
		.endif
		move	sp, k1
#endif
		.if \docfi
		CFI_DEF_CFA sp,0
		.endif
		cfi_st	k0, PT_R29, \docfi
		cfi_rel_offset  sp, PT_R29, \docfi
		cfi_st	v1, PT_R3, \docfi
		/*
		 * You might think that you don't need to save $0,
		 * but the FPU emulator and gdb remote debug stub
		 * need it to operate correctly
		 */
		LONG_S	$0, PT_R0(sp)
		mfc0	v1, CP0_STATUS
		cfi_st	v0, PT_R2, \docfi
		LONG_S	v1, PT_STATUS(sp)
#ifdef CONFIG_MIPS_MT_SMTC
		/*
		 * Ideally, these instructions would be shuffled in
		 * to cover the pipeline delay.
		 */
		.set	mips32
		mfc0	k0, CP0_TCSTATUS
		.set	mips0
		LONG_S	k0, PT_TCSTATUS(sp)
#endif /* CONFIG_MIPS_MT_SMTC */
		cfi_st	$4, PT_R4, \docfi
		mfc0	v1, CP0_CAUSE
		cfi_st	$5, PT_R5, \docfi
		LONG_S	v1, PT_CAUSE(sp)
		cfi_st	$6, PT_R6, \docfi
		MFC0	v1, CP0_EPC
		cfi_st	$7, PT_R7, \docfi
#ifdef CONFIG_64BIT
		cfi_st	$8, PT_R8, \docfi
		cfi_st	$9, PT_R9, \docfi
#endif
		LONG_S	v1, PT_EPC(sp)
		.if \docfi
		CFI_REL_OFFSET ra, PT_EPC
		.endif
		cfi_st	$25, PT_R25, \docfi
		cfi_st	$28, PT_R28, \docfi
		cfi_st	$31, PT_R31, \docfi
		ori	$28, sp, _THREAD_MASK
		xori	$28, _THREAD_MASK
#ifdef CONFIG_CPU_CAVIUM_OCTEON
		.set	mips64
		pref	0, 0($28)	/* Prefetch the current pointer */
		pref	0, PT_R31(sp)	/* Prefetch the $31(ra) */
#endif
		.set	pop
		.endm

		.macro	SAVE_ALL docfi=0
		SAVE_SOME \docfi
		SAVE_AT \docfi
		SAVE_TEMP \docfi
		SAVE_STATIC \docfi
		.endm

		.macro	RESTORE_AT docfi=0
		.set	push
		.set	noat
		cfi_ld	$1, PT_R1, \docfi
		.set	pop
		.endm

		.macro	RESTORE_TEMP docfi=0
#ifdef CONFIG_CPU_CAVIUM_OCTEON
		/* Restore the Octeon multiplier state */
		jal	octeon_mult_restore
#endif
#ifdef CONFIG_CPU_HAS_SMARTMIPS
		LONG_L	$24, PT_ACX(sp)
		mtlhx	$24
		LONG_L	$24, PT_HI(sp)
		mtlhx	$24
		LONG_L	$24, PT_LO(sp)
		mtlhx	$24
#else
		LONG_L	$24, PT_LO(sp)
		mtlo	$24
		LONG_L	$24, PT_HI(sp)
		mthi	$24
#endif
#ifdef CONFIG_32BIT
		cfi_ld	$8, PT_R8, \docfi
		cfi_ld	$9, PT_R9, \docfi
#endif
		cfi_ld	$10, PT_R10, \docfi
		cfi_ld	$11, PT_R11, \docfi
		cfi_ld	$12, PT_R12, \docfi
		cfi_ld	$13, PT_R13, \docfi
		cfi_ld	$14, PT_R14, \docfi
		cfi_ld	$15, PT_R15, \docfi
		cfi_ld	$24, PT_R24, \docfi
		.endm

		.macro	RESTORE_STATIC docfi=0
		cfi_ld	$16, PT_R16, \docfi
		cfi_ld	$17, PT_R17, \docfi
		cfi_ld	$18, PT_R18, \docfi
		cfi_ld	$19, PT_R19, \docfi
		cfi_ld	$20, PT_R20, \docfi
		cfi_ld	$21, PT_R21, \docfi
		cfi_ld	$22, PT_R22, \docfi
		cfi_ld	$23, PT_R23, \docfi
		cfi_ld	$30, PT_R30, \docfi
		.endm

#if defined(CONFIG_CPU_R3000) || defined(CONFIG_CPU_TX39XX)

		.macro	RESTORE_SOME docfi=0
		.set	push
		.set	reorder
		.set	noat
		mfc0	a0, CP0_STATUS
		li	v1, 0xff00
		ori	a0, STATMASK
		xori	a0, STATMASK
		mtc0	a0, CP0_STATUS
		and	a0, v1
		LONG_L	v0, PT_STATUS(sp)
		nor	v1, $0, v1
		and	v0, v1
		or	v0, a0
		mtc0	v0, CP0_STATUS
		cfi_ld	$31, PT_R31, \docfi
		cfi_ld	$28, PT_R28, \docfi
		cfi_ld	$25, PT_R25, \docfi
		cfi_ld	$7,  PT_R7, \docfi
		cfi_ld	$6,  PT_R6, \docfi
		cfi_ld	$5,  PT_R5, \docfi
		cfi_ld	$4,  PT_R4, \docfi
		cfi_ld	$3,  PT_R3, \docfi
		cfi_ld	$2,  PT_R2, \docfi
		.set	pop
		.endm

		.macro	RESTORE_SP_AND_RET docfi=0
		.set	push
		.set	noreorder
		LONG_L	k0, PT_EPC(sp)
		cfi_ld	sp, PT_R29, \docfi
		jr	k0
		 rfe
		.set	pop
		.endm

#else
		.macro	RESTORE_SOME docfi=0
		.set	push
		.set	reorder
		.set	noat
#ifdef CONFIG_MIPS_MT_SMTC
		.set	mips32r2
		/*
		 * We need to make sure the read-modify-write
		 * of Status below isn't perturbed by an interrupt
		 * or cross-TC access, so we need to do at least a DMT,
		 * protected by an interrupt-inhibit. But setting IXMT
		 * also creates a few-cycle window where an IPI could
		 * be queued and not be detected before potentially
		 * returning to a WAIT or user-mode loop. It must be
		 * replayed.
		 *
		 * We're in the middle of a context switch, and
		 * we can't dispatch it directly without trashing
		 * some registers, so we'll try to detect this unlikely
		 * case and program a software interrupt in the VPE,
		 * as would be done for a cross-VPE IPI.  To accommodate
		 * the handling of that case, we're doing a DVPE instead
		 * of just a DMT here to protect against other threads.
		 * This is a lot of cruft to cover a tiny window.
		 * If you can find a better design, implement it!
		 *
		 */
		mfc0	v0, CP0_TCSTATUS
		ori	v0, TCSTATUS_IXMT
		mtc0	v0, CP0_TCSTATUS
		_ehb
		DVPE	5				# dvpe a1
		jal	mips_ihb
#endif /* CONFIG_MIPS_MT_SMTC */
		mfc0	a0, CP0_STATUS
		ori	a0, STATMASK
		xori	a0, STATMASK
		mtc0	a0, CP0_STATUS
		li	v1, 0xff00
		and	a0, v1
		LONG_L	v0, PT_STATUS(sp)
		nor	v1, $0, v1
		and	v0, v1
		or	v0, a0
		mtc0	v0, CP0_STATUS
#ifdef CONFIG_MIPS_MT_SMTC
/*
 * Only after EXL/ERL have been restored to status can we
 * restore TCStatus.IXMT.
 */
		LONG_L	v1, PT_TCSTATUS(sp)
		_ehb
		mfc0	a0, CP0_TCSTATUS
		andi	v1, TCSTATUS_IXMT
		bnez	v1, 0f

/*
 * We'd like to detect any IPIs queued in the tiny window
 * above and request an software interrupt to service them
 * when we ERET.
 *
 * Computing the offset into the IPIQ array of the executing
 * TC's IPI queue in-line would be tedious.  We use part of
 * the TCContext register to hold 16 bits of offset that we
 * can add in-line to find the queue head.
 */
		mfc0	v0, CP0_TCCONTEXT
		la	a2, IPIQ
		srl	v0, v0, 16
		addu	a2, a2, v0
		LONG_L	v0, 0(a2)
		beqz	v0, 0f
/*
 * If we have a queue, provoke dispatch within the VPE by setting C_SW1
 */
		mfc0	v0, CP0_CAUSE
		ori	v0, v0, C_SW1
		mtc0	v0, CP0_CAUSE
0:
		/*
		 * This test should really never branch but
		 * let's be prudent here.  Having atomized
		 * the shared register modifications, we can
		 * now EVPE, and must do so before interrupts
		 * are potentially re-enabled.
		 */
		andi	a1, a1, MVPCONTROL_EVP
		beqz	a1, 1f
		evpe
1:
		/* We know that TCStatua.IXMT should be set from above */
		xori	a0, a0, TCSTATUS_IXMT
		or	a0, a0, v1
		mtc0	a0, CP0_TCSTATUS
		_ehb

		.set	mips0
#endif /* CONFIG_MIPS_MT_SMTC */
		LONG_L	v1, PT_EPC(sp)
		cfi_ld	$25, PT_R25, \docfi
		MTC0	v1, CP0_EPC

		cfi_ld	$31, PT_R31, \docfi
		cfi_ld	$28, PT_R28, \docfi
#ifdef CONFIG_64BIT
		cfi_ld	$8, PT_R8, \docfi
		cfi_ld	$9, PT_R9, \docfi
#endif
		cfi_ld	$7,  PT_R7, \docfi
		cfi_ld	$6,  PT_R6, \docfi
		cfi_ld	$5,  PT_R5, \docfi
		cfi_ld	$4,  PT_R4, \docfi
		cfi_ld	$3,  PT_R3, \docfi
		cfi_ld	$2,  PT_R2, \docfi
		.set	pop
		.endm

		.macro	RESTORE_SP docfi=0
		cfi_ld	sp, PT_R29, \docfi
		.endm

		.macro	RESTORE_SP_AND_RET docfi=0
		LONG_L	k0, PT_R26(sp)
		LONG_L	k1, PT_R27(sp)
#ifdef CONFIG_FAST_ACCESS_TO_THREAD_POINTER
		LONG_L	k0, FAST_ACCESS_THREAD_OFFSET($0) /* K0 = thread pointer */
#endif
		RESTORE_SP \docfi
		.set	mips3
		eret
		.set	mips0
		.endm

#endif

		.macro	RESTORE_ALL docfi=0
		RESTORE_TEMP \docfi
		RESTORE_STATIC \docfi
		RESTORE_AT \docfi
		RESTORE_SOME \docfi
		RESTORE_SP \docfi
		.endm

		.macro	RESTORE_ALL_AND_RET docfi=0
		RESTORE_TEMP \docfi
		RESTORE_STATIC \docfi
		RESTORE_AT \docfi
		RESTORE_SOME \docfi
		RESTORE_SP_AND_RET \docfi
		.endm

/*
 * Move to kernel mode and disable interrupts.
 * Set cp0 enable bit as sign that we're running on the kernel stack
 */
		.macro	CLI
#if !defined(CONFIG_MIPS_MT_SMTC)
		mfc0	t0, CP0_STATUS
		li	t1, ST0_CU0 | STATMASK
		or	t0, t1
		xori	t0, STATMASK
		mtc0	t0, CP0_STATUS
#else /* CONFIG_MIPS_MT_SMTC */
		/*
		 * For SMTC, we need to set privilege
		 * and disable interrupts only for the
		 * current TC, using the TCStatus register.
		 */
		mfc0	t0, CP0_TCSTATUS
		/* Fortunately CU 0 is in the same place in both registers */
		/* Set TCU0, TMX, TKSU (for later inversion) and IXMT */
		li	t1, ST0_CU0 | 0x08001c00
		or	t0, t1
		/* Clear TKSU, leave IXMT */
		xori	t0, 0x00001800
		mtc0	t0, CP0_TCSTATUS
		_ehb
		/* We need to leave the global IE bit set, but clear EXL...*/
		mfc0	t0, CP0_STATUS
		ori	t0, ST0_EXL | ST0_ERL
		xori	t0, ST0_EXL | ST0_ERL
		mtc0	t0, CP0_STATUS
#endif /* CONFIG_MIPS_MT_SMTC */
		irq_disable_hazard
		.endm

/*
 * Move to kernel mode and enable interrupts.
 * Set cp0 enable bit as sign that we're running on the kernel stack
 */
		.macro	STI
#if !defined(CONFIG_MIPS_MT_SMTC)
		mfc0	t0, CP0_STATUS
		li	t1, ST0_CU0 | STATMASK
		or	t0, t1
		xori	t0, STATMASK & ~1
		mtc0	t0, CP0_STATUS
#else /* CONFIG_MIPS_MT_SMTC */
		/*
		 * For SMTC, we need to set privilege
		 * and enable interrupts only for the
		 * current TC, using the TCStatus register.
		 */
		_ehb
		mfc0	t0, CP0_TCSTATUS
		/* Fortunately CU 0 is in the same place in both registers */
		/* Set TCU0, TKSU (for later inversion) and IXMT */
		li	t1, ST0_CU0 | 0x08001c00
		or	t0, t1
		/* Clear TKSU *and* IXMT */
		xori	t0, 0x00001c00
		mtc0	t0, CP0_TCSTATUS
		_ehb
		/* We need to leave the global IE bit set, but clear EXL...*/
		mfc0	t0, CP0_STATUS
		ori	t0, ST0_EXL
		xori	t0, ST0_EXL
		mtc0	t0, CP0_STATUS
		/* irq_enable_hazard below should expand to EHB for 24K/34K cpus */
#endif /* CONFIG_MIPS_MT_SMTC */
		irq_enable_hazard
		.endm

/*
 * Just move to kernel mode and leave interrupts as they are.  Note
 * for the R3000 this means copying the previous enable from IEp.
 * Set cp0 enable bit as sign that we're running on the kernel stack
 */
		.macro	KMODE
#ifdef CONFIG_MIPS_MT_SMTC
		/*
		 * This gets baroque in SMTC.  We want to
		 * protect the non-atomic clearing of EXL
		 * with DMT/EMT, but we don't want to take
		 * an interrupt while DMT is still in effect.
		 */

		/* KMODE gets invoked from both reorder and noreorder code */
		.set	push
		.set	mips32r2
		.set	noreorder
		mfc0	v0, CP0_TCSTATUS
		andi	v1, v0, TCSTATUS_IXMT
		ori	v0, TCSTATUS_IXMT
		mtc0	v0, CP0_TCSTATUS
		_ehb
		DMT	2				# dmt	v0
		/*
		 * We don't know a priori if ra is "live"
		 */
		move	t0, ra
		jal	mips_ihb
		nop	/* delay slot */
		move	ra, t0
#endif /* CONFIG_MIPS_MT_SMTC */
		mfc0	t0, CP0_STATUS
		li	t1, ST0_CU0 | (STATMASK & ~1)
#if defined(CONFIG_CPU_R3000) || defined(CONFIG_CPU_TX39XX)
		andi	t2, t0, ST0_IEP
		srl	t2, 2
		or	t0, t2
#endif
		or	t0, t1
		xori	t0, STATMASK & ~1
		mtc0	t0, CP0_STATUS
#ifdef CONFIG_MIPS_MT_SMTC
		_ehb
		andi	v0, v0, VPECONTROL_TE
		beqz	v0, 2f
		nop	/* delay slot */
		emt
2:
		mfc0	v0, CP0_TCSTATUS
		/* Clear IXMT, then OR in previous value */
		ori	v0, TCSTATUS_IXMT
		xori	v0, TCSTATUS_IXMT
		or	v0, v1, v0
		mtc0	v0, CP0_TCSTATUS
		/*
		 * irq_disable_hazard below should expand to EHB
		 * on 24K/34K CPUS
		 */
		.set pop
#endif /* CONFIG_MIPS_MT_SMTC */
		irq_disable_hazard
		.endm

#endif /* _ASM_STACKFRAME_H */
