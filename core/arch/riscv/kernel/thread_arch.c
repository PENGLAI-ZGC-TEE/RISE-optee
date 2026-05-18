// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2022-2023 NXP
 * Copyright (c) 2016-2022, Linaro Limited
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * Copyright (c) 2020-2021, Arm Limited
 */

#include <platform_config.h>

#include <assert.h>
#include <config.h>
#include <drivers/plic.h>
#include <io.h>
#include <keep.h>
#include <kernel/asan.h>
#include <kernel/boot.h>
#include <kernel/interrupt.h>
#include <kernel/linker.h>
#include <kernel/lockdep.h>
#include <kernel/misc.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <kernel/tee_ta_manager.h>
#include <kernel/thread.h>
#include <kernel/thread_private.h>
#include <kernel/user_mode_ctx_struct.h>
#include <kernel/virtualization.h>
#include <mm/core_memprot.h>
#include <mm/mobj.h>
#include <mm/tee_mm.h>
#include <mm/vm.h>
#include <riscv.h>
#include <trace.h>
#include <util.h>

/*
 * This function is called as a guard after each ABI call which is not
 * supposed to return.
 */
void __noreturn __panic_at_abi_return(void)
{
	panic();
}

/* This function returns current masked exception bits. */
uint32_t __nostackcheck thread_get_exceptions(void)
{
	uint32_t xie = read_csr(CSR_XIE) & THREAD_EXCP_ALL;

	return xie ^ THREAD_EXCP_ALL;
}

void __nostackcheck thread_set_exceptions(uint32_t exceptions)
{
	/* Foreign interrupts must not be unmasked while holding a spinlock */
	if (!(exceptions & THREAD_EXCP_FOREIGN_INTR))
		assert_have_no_spinlock();

	/*
	 * In ARM, the bits in DAIF register are used to mask the exceptions.
	 * While in RISC-V, the bits in CSR XIE are used to enable(unmask)
	 * corresponding interrupt sources. To not modify the function of
	 * thread_set_exceptions(), we should "invert" the bits in "exceptions".
	 * The corresponding bits in "exceptions" will be inverted so they will
	 * be cleared when we write the final value into CSR XIE. So that we
	 * can mask those exceptions.
	 */
	exceptions &= THREAD_EXCP_ALL;
	exceptions ^= THREAD_EXCP_ALL;

	barrier();
	write_csr(CSR_XIE, exceptions);
	barrier();
}

uint32_t __nostackcheck thread_mask_exceptions(uint32_t exceptions)
{
	uint32_t state = thread_get_exceptions();

	thread_set_exceptions(state | (exceptions & THREAD_EXCP_ALL));
	return state;
}

void __nostackcheck thread_unmask_exceptions(uint32_t state)
{
	thread_set_exceptions(state & THREAD_EXCP_ALL);
}

static void thread_lazy_save_ns_vfp(void)
{
	static_assert(!IS_ENABLED(CFG_WITH_VFP));
}

static void thread_lazy_restore_ns_vfp(void)
{
	static_assert(!IS_ENABLED(CFG_WITH_VFP));
}

static void setup_unwind_user_mode(struct thread_scall_regs *regs)
{
	regs->epc = (uintptr_t)thread_unwind_user_mode;
	regs->status = xstatus_for_xret(true, PRV_S);
	regs->ie = 0;
	/*
	 * We are going to exit user mode. The stack pointer must be set as the
	 * original value it had before allocating space of scall "regs" and
	 * calling thread_scall_handler(). Thus, we can simply set stack pointer
	 * as (regs + 1) value.
	 */
	regs->sp = (uintptr_t)(regs + 1);
}

static void thread_unhandled_trap(struct thread_ctx_regs *regs __unused,
				  unsigned long cause __unused)
{
	DMSG("Unhandled trap xepc:0x%016lx xcause:0x%016lx xtval:0x%016lx",
	     read_csr(CSR_XEPC), read_csr(CSR_XCAUSE), read_csr(CSR_XTVAL));
	panic();
}

void thread_scall_handler(struct thread_scall_regs *regs)
{
	struct ts_session *sess = NULL;
	uint32_t state = 0;

	/* Enable native interrupts */
	state = thread_get_exceptions();
	thread_unmask_exceptions(state & ~THREAD_EXCP_NATIVE_INTR);

	thread_user_save_vfp();

	sess = ts_get_current_session();

	/* Restore foreign interrupts which are disabled on exception entry */
	thread_restore_foreign_intr();

	assert(sess && sess->handle_scall);

	if (sess->handle_scall(regs)) {
		/*
		 * We're about to switch back to next instruction of ecall in
		 * user-mode
		 */
		regs->epc += 4;
	} else {
		/* We're returning from __thread_enter_user_mode() */
		setup_unwind_user_mode(regs);
	}
}

static void thread_irq_handler(void)
{
	interrupt_main_handler();
}

struct nanhu_thread_regs_snapshot {
	unsigned long epc;
	unsigned long status;
	unsigned long ie;
	unsigned long ra;
	unsigned long sp;
	unsigned long t0;
	unsigned long t1;
	unsigned long t2;
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
	unsigned long a4;
	unsigned long a5;
	unsigned long a6;
	unsigned long a7;
	unsigned long s0;
	unsigned long s1;
	unsigned long s2;
	unsigned long s3;
	unsigned long s4;
	unsigned long s5;
	unsigned long s6;
	unsigned long s7;
	unsigned long s8;
	unsigned long s9;
	unsigned long s10;
	unsigned long s11;
	unsigned long t3;
	unsigned long t4;
	unsigned long t5;
	unsigned long t6;
};

static void nanhu_thread_snapshot(struct nanhu_thread_regs_snapshot *snap,
				  const struct thread_ctx_regs *regs)
{
	if (!snap || !regs)
		return;

	snap->epc = regs->epc;
	snap->status = regs->status;
	snap->ie = regs->ie;
	snap->ra = regs->ra;
	snap->sp = regs->sp;
	snap->t0 = regs->t0;
	snap->t1 = regs->t1;
	snap->t2 = regs->t2;
	snap->a0 = regs->a0;
	snap->a1 = regs->a1;
	snap->a2 = regs->a2;
	snap->a3 = regs->a3;
	snap->a4 = regs->a4;
	snap->a5 = regs->a5;
	snap->a6 = regs->a6;
	snap->a7 = regs->a7;
	snap->s0 = regs->s0;
	snap->s1 = regs->s1;
	snap->s2 = regs->s2;
	snap->s3 = regs->s3;
	snap->s4 = regs->s4;
	snap->s5 = regs->s5;
	snap->s6 = regs->s6;
	snap->s7 = regs->s7;
	snap->s8 = regs->s8;
	snap->s9 = regs->s9;
	snap->s10 = regs->s10;
	snap->s11 = regs->s11;
	snap->t3 = regs->t3;
	snap->t4 = regs->t4;
	snap->t5 = regs->t5;
	snap->t6 = regs->t6;
}

static bool nanhu_thread_snapshot_equal(const struct nanhu_thread_regs_snapshot *a,
					const struct nanhu_thread_regs_snapshot *b)
{
	return a->epc == b->epc && a->status == b->status &&
	       a->ie == b->ie && a->ra == b->ra && a->sp == b->sp &&
	       a->t0 == b->t0 && a->t1 == b->t1 && a->t2 == b->t2 &&
	       a->a0 == b->a0 && a->a1 == b->a1 && a->a2 == b->a2 &&
	       a->a3 == b->a3 && a->a4 == b->a4 && a->a5 == b->a5 &&
	       a->a6 == b->a6 && a->a7 == b->a7 &&
	       a->s0 == b->s0 && a->s1 == b->s1 && a->s2 == b->s2 &&
	       a->s3 == b->s3 && a->s4 == b->s4 && a->s5 == b->s5 &&
	       a->s6 == b->s6 && a->s7 == b->s7 && a->s8 == b->s8 &&
	       a->s9 == b->s9 && a->s10 == b->s10 && a->s11 == b->s11 &&
	       a->t3 == b->t3 && a->t4 == b->t4 &&
	       a->t5 == b->t5 && a->t6 == b->t6;
}

static void nanhu_thread_trace_regs(const char *tag,
				    const struct thread_ctx_regs *regs,
				    unsigned long cause)
{
	if (!regs)
		return;

	IMSG("[TEE-IRQ] %s test=%s cause=0x%lx epc=0x%lx status=0x%lx ie=0x%lx",
	     tag, plic_nanhu_trace_test(), cause, regs->epc, regs->status,
	     regs->ie);
	IMSG("[TEE-IRQ] GPR test=%s ra=0x%lx sp=0x%lx gp=0x%lx tp=0x%lx a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx a4=0x%lx a5=0x%lx a6=0x%lx a7=0x%lx",
	     plic_nanhu_trace_test(), regs->ra, regs->sp, regs->gp, regs->tp,
	     regs->a0, regs->a1, regs->a2, regs->a3, regs->a4, regs->a5,
	     regs->a6, regs->a7);
	IMSG("[TEE-IRQ] GPR test=%s s0=0x%lx s1=0x%lx s2=0x%lx s3=0x%lx s4=0x%lx s5=0x%lx s6=0x%lx s7=0x%lx s8=0x%lx s9=0x%lx s10=0x%lx s11=0x%lx",
	     plic_nanhu_trace_test(), regs->s0, regs->s1, regs->s2,
	     regs->s3, regs->s4, regs->s5, regs->s6, regs->s7, regs->s8,
	     regs->s9, regs->s10, regs->s11);
	IMSG("[TEE-IRQ] GPR test=%s t0=0x%lx t1=0x%lx t2=0x%lx t3=0x%lx t4=0x%lx t5=0x%lx t6=0x%lx",
	     plic_nanhu_trace_test(), regs->t0, regs->t1, regs->t2,
	     regs->t3, regs->t4, regs->t5, regs->t6);
}

void thread_native_interrupt_handler(struct thread_ctx_regs *regs,
				     unsigned long cause)
{
	bool trace = plic_nanhu_trace_active() && (cause & LONG_MAX) == IRQ_XEXT;
	struct nanhu_thread_regs_snapshot entry = { };
	struct nanhu_thread_regs_snapshot exit = { };

	if (trace) {
		nanhu_thread_snapshot(&entry, regs);
		nanhu_thread_trace_regs("SAVE", regs, cause);
	}

	switch (cause & LONG_MAX) {
	case IRQ_XTIMER:
		clear_csr(CSR_XIE, CSR_XIE_TIE);
		break;
	case IRQ_XSOFT:
		thread_unhandled_trap(regs, cause);
		break;
	case IRQ_XEXT:
		thread_irq_handler();
		break;
	default:
		thread_unhandled_trap(regs, cause);
	}

	if (trace) {
		nanhu_thread_snapshot(&exit, regs);
		IMSG("[TEE-IRQ] RESTORE-CHECK test=%s result=%s epc=0x%lx status=0x%lx ie=0x%lx",
		     plic_nanhu_trace_test(),
		     nanhu_thread_snapshot_equal(&entry, &exit) ? "OK" : "CHANGED",
		     regs->epc, regs->status, regs->ie);
	}
}

unsigned long xstatus_for_xret(uint8_t pie, uint8_t pp)
{
	unsigned long xstatus = read_csr(CSR_XSTATUS);

	assert(pp == PRV_M || pp == PRV_S || pp == PRV_U);

#ifdef RV32
	xstatus = set_field_u32(xstatus, CSR_XSTATUS_IE, 0);
	xstatus = set_field_u32(xstatus, CSR_XSTATUS_PIE, pie);
	xstatus = set_field_u32(xstatus, CSR_XSTATUS_SPP, pp);
#else	/* RV64 */
	xstatus = set_field_u64(xstatus, CSR_XSTATUS_IE, 0);
	xstatus = set_field_u64(xstatus, CSR_XSTATUS_PIE, pie);
	xstatus = set_field_u64(xstatus, CSR_XSTATUS_SPP, pp);
#endif

	return xstatus;
}

static void init_regs(struct thread_ctx *thread, uint32_t a0, uint32_t a1,
		      uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
		      uint32_t a6, uint32_t a7, void *pc)
{
	memset(&thread->regs, 0, sizeof(thread->regs));

	thread->regs.epc = (uintptr_t)pc;

	/* Set up xstatus */
	thread->regs.status = xstatus_for_xret(true, PRV_S);

	/* Enable native interrupt */
	thread->regs.ie = THREAD_EXCP_NATIVE_INTR;

	/* Reinitialize stack pointer */
	thread->regs.sp = thread->stack_va_end;

	/* Set up GP and TP */
	thread->regs.gp = read_gp();
	thread->regs.tp = read_tp();

	/*
	 * Copy arguments into context. This will make the
	 * arguments appear in a0-a7 when thread is started.
	 */
	thread->regs.a0 = a0;
	thread->regs.a1 = a1;
	thread->regs.a2 = a2;
	thread->regs.a3 = a3;
	thread->regs.a4 = a4;
	thread->regs.a5 = a5;
	thread->regs.a6 = a6;
	thread->regs.a7 = a7;
}

static void __thread_alloc_and_run(uint32_t a0, uint32_t a1, uint32_t a2,
				   uint32_t a3, uint32_t a4, uint32_t a5,
				   uint32_t a6, uint32_t a7,
				   void *pc)
{
	struct thread_core_local *l = thread_get_core_local();
	bool found_thread = false;
	size_t n = 0;

	assert(l->curr_thread == THREAD_ID_INVALID);

	thread_lock_global();

	for (n = 0; n < CFG_NUM_THREADS; n++) {
		if (threads[n].state == THREAD_STATE_FREE) {
			threads[n].state = THREAD_STATE_ACTIVE;
			found_thread = true;
			break;
		}
	}

	thread_unlock_global();

	if (!found_thread)
		return;

	l->curr_thread = n;

	threads[n].flags = 0;
	init_regs(threads + n, a0, a1, a2, a3, a4, a5, a6, a7, pc);

	thread_lazy_save_ns_vfp();

	l->flags &= ~THREAD_CLF_TMP;

	thread_resume(&threads[n].regs);
	/*NOTREACHED*/
	panic();
}

void thread_alloc_and_run(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
			  uint32_t a4, uint32_t a5)
{
	__thread_alloc_and_run(a0, a1, a2, a3, a4, a5, 0, 0,
			       thread_std_abi_entry);
}

static void copy_a0_to_a3(struct thread_ctx_regs *regs, uint32_t a0,
			  uint32_t a1, uint32_t a2, uint32_t a3)
{
	regs->a0 = a0;
	regs->a1 = a1;
	regs->a2 = a2;
	regs->a3 = a3;
}

static bool is_from_user(unsigned long status)
{
	return (status & CSR_XSTATUS_SPP) == 0;
}

#ifdef CFG_SYSCALL_FTRACE
static void __noprof ftrace_suspend(void)
{
	struct ts_session *s = TAILQ_FIRST(&thread_get_tsd()->sess_stack);

	if (s && s->fbuf)
		s->fbuf->syscall_trace_suspended = true;
}

static void __noprof ftrace_resume(void)
{
	struct ts_session *s = TAILQ_FIRST(&thread_get_tsd()->sess_stack);

	if (s && s->fbuf)
		s->fbuf->syscall_trace_suspended = false;
}
#else
static void __maybe_unused __noprof ftrace_suspend(void)
{
}

static void __noprof ftrace_resume(void)
{
}
#endif

static bool is_user_mode(struct thread_ctx_regs *regs)
{
	return is_from_user((uint32_t)regs->status);
}

vaddr_t thread_get_saved_thread_sp(void)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != THREAD_ID_INVALID);
	return threads[ct].kern_sp;
}

void thread_resume_from_rpc(uint32_t thread_id, uint32_t a0, uint32_t a1,
			    uint32_t a2, uint32_t a3)
{
	size_t n = thread_id;
	struct thread_core_local *l = thread_get_core_local();
	bool found_thread = false;

	assert(l->curr_thread == THREAD_ID_INVALID);

	thread_lock_global();

	if (n < CFG_NUM_THREADS && threads[n].state == THREAD_STATE_SUSPENDED) {
		threads[n].state = THREAD_STATE_ACTIVE;
		found_thread = true;
	}

	thread_unlock_global();

	if (!found_thread)
		return;

	l->curr_thread = n;

	if (threads[n].have_user_map) {
		core_mmu_set_user_map(&threads[n].user_map);
		if (threads[n].flags & THREAD_FLAGS_EXIT_ON_FOREIGN_INTR)
			tee_ta_ftrace_update_times_resume();
	}

	if (is_user_mode(&threads[n].regs))
		tee_ta_update_session_utime_resume();

	/*
	 * We may resume thread at another hart, so we need to re-assign value
	 * of tp to be current hart's thread_core_local.
	 */
	if (!is_user_mode(&threads[n].regs))
		threads[n].regs.tp = read_tp();

	/*
	 * Return from RPC to request service of a foreign interrupt must not
	 * get parameters from non-secure world.
	 */
	if (threads[n].flags & THREAD_FLAGS_COPY_ARGS_ON_RETURN) {
		copy_a0_to_a3(&threads[n].regs, a0, a1, a2, a3);
		threads[n].flags &= ~THREAD_FLAGS_COPY_ARGS_ON_RETURN;
	}

	thread_lazy_save_ns_vfp();

	if (threads[n].have_user_map)
		ftrace_resume();

	l->flags &= ~THREAD_CLF_TMP;
	thread_resume(&threads[n].regs);
	/*NOTREACHED*/
	panic();
}

void thread_state_free(void)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != THREAD_ID_INVALID);

	thread_lazy_restore_ns_vfp();

	thread_lock_global();

	assert(threads[ct].state == THREAD_STATE_ACTIVE);
	threads[ct].state = THREAD_STATE_FREE;
	threads[ct].flags = 0;
	l->curr_thread = THREAD_ID_INVALID;

	if (IS_ENABLED(CFG_NS_VIRTUALIZATION))
		virt_unset_guest();
	thread_unlock_global();
}

int thread_state_suspend(uint32_t flags, unsigned long status, vaddr_t pc)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != THREAD_ID_INVALID);

	if (core_mmu_user_mapping_is_active())
		ftrace_suspend();

	thread_check_canaries();

	if (is_from_user(status)) {
		thread_user_save_vfp();
		tee_ta_update_session_utime_suspend();
		tee_ta_gprof_sample_pc(pc);
	}
	thread_lazy_restore_ns_vfp();

	thread_lock_global();

	assert(threads[ct].state == THREAD_STATE_ACTIVE);
	threads[ct].flags |= flags;
	threads[ct].regs.status = status;
	threads[ct].regs.epc = pc;
	threads[ct].state = THREAD_STATE_SUSPENDED;

	threads[ct].have_user_map = core_mmu_user_mapping_is_active();
	if (threads[ct].have_user_map) {
		if (threads[ct].flags & THREAD_FLAGS_EXIT_ON_FOREIGN_INTR)
			tee_ta_ftrace_update_times_suspend();
		core_mmu_get_user_map(&threads[ct].user_map);
		core_mmu_set_user_map(NULL);
	}

	l->curr_thread = THREAD_ID_INVALID;

	if (IS_ENABLED(CFG_NS_VIRTUALIZATION))
		virt_unset_guest();

	thread_unlock_global();

	return ct;
}

static void init_user_kcode(void)
{
}

void thread_init_primary(void)
{
	/* Initialize canaries around the stacks */
	thread_init_canaries();

	init_user_kcode();
}

static vaddr_t get_trap_vect(void)
{
	return (vaddr_t)thread_trap_vect;
}

void thread_init_tvec(void)
{
	unsigned long tvec = (unsigned long)get_trap_vect();

	write_csr(CSR_XTVEC, tvec);
	assert(read_csr(CSR_XTVEC) == tvec);
}

void thread_init_per_cpu(void)
{
	thread_init_tvec();
	/*
	 * We may receive traps from now, therefore, zeroize xSCRATCH such
	 * that thread_trap_vect() can distinguish between user traps
	 * and kernel traps.
	 */
	write_csr(CSR_XSCRATCH, 0);
#ifndef CFG_PAN
	/*
	 * Allow access to user pages. When CFG_PAN is enabled, the SUM bit will
	 * be set and clear at runtime when necessary.
	 */
	set_csr(CSR_XSTATUS, CSR_XSTATUS_SUM);
#endif
}

static void set_ctx_regs(struct thread_ctx_regs *regs, unsigned long a0,
			 unsigned long a1, unsigned long a2, unsigned long a3,
			 unsigned long user_sp, unsigned long entry_func,
			 unsigned long status, unsigned long ie,
			 struct thread_pauth_keys *keys __unused)
{
	*regs = (struct thread_ctx_regs){
		.a0 = a0,
		.a1 = a1,
		.a2 = a2,
		.a3 = a3,
		.s0 = 0,
		.sp = user_sp,
		.epc = entry_func,
		.status = status,
		.ie = ie,
	};
}

uint32_t thread_enter_user_mode(unsigned long a0, unsigned long a1,
				unsigned long a2, unsigned long a3,
				unsigned long user_sp,
				unsigned long entry_func,
				bool is_32bit __unused,
				uint32_t *exit_status0,
				uint32_t *exit_status1)
{
	unsigned long status = 0;
	unsigned long ie = 0;
	uint32_t exceptions = 0;
	uint32_t rc = 0;
	struct thread_ctx_regs *regs = NULL;

	tee_ta_update_session_utime_resume();

	/* Read current interrupt masks */
	ie = read_csr(CSR_XIE);

	/*
	 * Mask all exceptions, the CSR_XSTATUS.IE will be set from
	 * setup_unwind_user_mode() after exiting.
	 */
	exceptions = thread_mask_exceptions(THREAD_EXCP_ALL);
	regs = thread_get_ctx_regs();
	status = xstatus_for_xret(true, PRV_U);
	set_ctx_regs(regs, a0, a1, a2, a3, user_sp, entry_func, status, ie,
		     NULL);
	rc = __thread_enter_user_mode(regs, exit_status0, exit_status1);
	thread_unmask_exceptions(exceptions);

	return rc;
}

void __thread_rpc(uint32_t rv[THREAD_RPC_NUM_ARGS])
{
	thread_rpc_xstatus(rv, xstatus_for_xret(false, PRV_S));
}
