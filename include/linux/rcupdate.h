/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * Copyright IBM Corporation, 2001
 *
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
 *
 * Based on the original work by Paul McKenney <paulmck@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */

#ifndef __LINUX_RCUPDATE_H
#define __LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/completion.h>
#include <linux/bottom_half.h>
#include <linux/lockdep.h>
#include <asm/processor.h>
#include <linux/cpumask.h>

bool rcu_irq_enter_disabled(void);

#define ULONG_CMP_GE(a, b)	(ULONG_MAX / 2 >= (a) - (b))
#define ULONG_CMP_LT(a, b)	(ULONG_MAX / 2 < (a) - (b))
#define ulong2long(a)		(*(long *)(&(a)))

/* Exported common interfaces */

#ifdef CONFIG_PREEMPT_RCU
void call_rcu(struct rcu_head *head, rcu_callback_t func);
#else /* #ifdef CONFIG_PREEMPT_RCU */
#define	call_rcu	call_rcu_sched
#endif /* #else #ifdef CONFIG_PREEMPT_RCU */

void call_rcu_bh(struct rcu_head *head, rcu_callback_t func);
void call_rcu_sched(struct rcu_head *head, rcu_callback_t func);
void synchronize_sched(void);

/**
 * call_rcu_tasks() - Queue an RCU for invocation task-based grace period
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual callback function to be invoked after the grace period
 *
 * The callback function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed. call_rcu_tasks() assumes
 * that the read-side critical sections end at a voluntary context
 * switch (not a preemption!), entry into idle, or transition to usermode
 * execution.  As such, there are no read-side primitives analogous to
 * rcu_read_lock() and rcu_read_unlock() because this primitive is intended
 * to determine that all tasks have passed through a safe state, not so
 * much for data-strcuture synchronization.
 *
 * See the description of call_rcu() for more detailed information on
 * memory ordering guarantees.
 */
void rcu_barrier_tasks(void);

#ifdef CONFIG_PREEMPT_RCU

void __rcu_read_lock(void);
void __rcu_read_unlock(void);
void synchronize_rcu(void);

/*
 * Defined as a macro as it is a very low level header included from
 * areas that don't even know about current.  This gives the rcu_read_lock()
 * nesting depth, but makes sense only if CONFIG_PREEMPT_RCU -- in other
 * types of kernel builds, the rcu_read_lock() nesting depth is unknowable.
 */
#define rcu_preempt_depth() (current->rcu_read_lock_nesting)

#else /* #ifdef CONFIG_PREEMPT_RCU */

static inline void __rcu_read_lock(void)
{
	preempt_disable();
}

static inline void __rcu_read_unlock(void)
{
	preempt_enable();
}

static inline void synchronize_rcu(void)
{
	synchronize_sched();
}

static inline int rcu_preempt_depth(void)
{
	return 0;
}

#endif /* #else #ifdef CONFIG_PREEMPT_RCU */

/* Internal to kernel */
void rcu_init(void);
extern int rcu_scheduler_active __read_mostly;
void rcu_sched_qs(void);
void rcu_bh_qs(void);
void rcu_check_callbacks(int user);
void rcu_report_dead(unsigned int cpu);
void rcutree_migrate_callbacks(int cpu);

#ifdef CONFIG_RCU_STALL_COMMON
void rcu_sysrq_start(void);
void rcu_sysrq_end(void);
#else /* #ifdef CONFIG_RCU_STALL_COMMON */
static inline void rcu_sysrq_start(void) { }
static inline void rcu_sysrq_end(void) { }
#endif /* #else #ifdef CONFIG_RCU_STALL_COMMON */

#ifdef CONFIG_NO_HZ_FULL
void rcu_user_enter(void);
void rcu_user_exit(void);
#else
static inline void rcu_user_enter(void) { }
static inline void rcu_user_exit(void) { }
#endif /* CONFIG_NO_HZ_FULL */

#ifdef CONFIG_RCU_NOCB_CPU
void rcu_init_nohz(void);
#else /* #ifdef CONFIG_RCU_NOCB_CPU */
static inline void rcu_init_nohz(void) { }
#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

/**
 * RCU_NONIDLE - Indicate idle-loop code that needs RCU readers
 * @a: Code that RCU needs to pay attention to.
 *
 * RCU, RCU-bh, and RCU-sched read-side critical sections are forbidden
 * in the inner idle loop, that is, between the rcu_idle_enter() and
 * the rcu_idle_exit() -- RCU will happily ignore any such read-side
 * critical sections.  However, things like powertop need tracepoints
 * in the inner idle loop.
 *
 * This macro provides the way out:  RCU_NONIDLE(do_something_with_RCU())
 * will tell RCU that it needs to pay attention, invoke its argument
 * (in this example, calling the do_something_with_RCU() function),
 * and then tell RCU to go back to ignoring this CPU.  It is permissible
 * to nest RCU_NONIDLE() wrappers, but not indefinitely (but the limit is
 * on the order of a million or so, even on 32-bit systems).  It is
 * not legal to block within RCU_NONIDLE(), nor is it permissible to
 * transfer control either into or out of RCU_NONIDLE()'s statement.
 */
#define RCU_NONIDLE(a) \
	do { \
		rcu_irq_enter_irqson(); \
		do { a; } while (0); \
		rcu_irq_exit_irqson(); \
	} while (0)

/*
 * Note a voluntary context switch for RCU-tasks benefit.  This is a
 * macro rather than an inline function to avoid #include hell.
 */
#ifdef CONFIG_TASKS_RCU
#define rcu_note_voluntary_context_switch_lite(t) \
	do { \
		if (READ_ONCE((t)->rcu_tasks_holdout)) \
			WRITE_ONCE((t)->rcu_tasks_holdout, false); \
	} while (0)
#define rcu_note_voluntary_context_switch(t) \
	do { \
		rcu_all_qs(); \
		rcu_note_voluntary_context_switch_lite(t); \
	} while (0)
void call_rcu_tasks(struct rcu_head *head, rcu_callback_t func);
void synchronize_rcu_tasks(void);
void exit_tasks_rcu_start(void);
void exit_tasks_rcu_finish(void);
#else /* #ifdef CONFIG_TASKS_RCU */
#define rcu_note_voluntary_context_switch_lite(t)	do { } while (0)
#define rcu_note_voluntary_context_switch(t)		rcu_all_qs()
#define call_rcu_tasks call_rcu_sched
#define synchronize_rcu_tasks synchronize_sched
static inline void exit_tasks_rcu_start(void) { }
static inline void exit_tasks_rcu_finish(void) { }
#endif /* #else #ifdef CONFIG_TASKS_RCU */

/**
 * cond_resched_tasks_rcu_qs - Report potential quiescent states to RCU
 *
 * This macro resembles cond_resched(), except that it is defined to
 * report potential quiescent states to RCU-tasks even if the cond_resched()
 * machinery were to be shut off, as some advocate for PREEMPT kernels.
 */
#define cond_resched_tasks_rcu_qs() \
do { \
	rcu_note_voluntary_context_switch_lite(current); \
	cond_resched(); \
} while (0)

/*
 * Infrastructure to implement the synchronize_() primitives in
 * TREE_RCU and rcu_barrier_() primitives in TINY_RCU.
 */

#if defined(CONFIG_TREE_RCU) || defined(CONFIG_PREEMPT_RCU)
#include <linux/rcutree.h>
#elif defined(CONFIG_TINY_RCU)
#include <linux/rcutiny.h>
#else
#error "Unknown RCU implementation specified to kernel configuration"
#endif

/*
 * init_rcu_head_on_stack()/destroy_rcu_head_on_stack() are needed for dynamic
 * initialization and destruction of rcu_head on the stack. rcu_head structures
 * allocated dynamically in the heap or defined statically don't need any
 * initialization.
 */
#ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD
void init_rcu_head(struct rcu_head *head);
void destroy_rcu_head(struct rcu_head *head);
void init_rcu_head_on_stack(struct rcu_head *head);
void destroy_rcu_head_on_stack(struct rcu_head *head);
#else /* !CONFIG_DEBUG_OBJECTS_RCU_HEAD */
static inline void init_rcu_head(struct rcu_head *head) { }
static inline void destroy_rcu_head(struct rcu_head *head) { }
static inline void init_rcu_head_on_stack(struct rcu_head *head) { }
static inline void destroy_rcu_head_on_stack(struct rcu_head *head) { }
#endif	/* #else !CONFIG_DEBUG_OBJECTS_RCU_HEAD */

#if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU)
bool rcu_lockdep_current_cpu_online(void);
#else /* #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU) */
static inline bool rcu_lockdep_current_cpu_online(void) { return true; }
#endif /* #else #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU) */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

static inline void rcu_lock_acquire(struct lockdep_map *map)
{
	lock_acquire(map, 0, 0, 2, 0, NULL, _THIS_IP_);
}

static inline void rcu_lock_release(struct lockdep_map *map)
{
	lock_release(map, 1, _THIS_IP_);
}

extern struct lockdep_map rcu_lock_map;
extern struct lockdep_map rcu_bh_lock_map;
extern struct lockdep_map rcu_sched_lock_map;
extern struct lockdep_map rcu_callback_map;
int debug_lockdep_rcu_enabled(void);
int rcu_read_lock_held(void);
int rcu_read_lock_bh_held(void);
int rcu_read_lock_sched_held(void);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

# define rcu_lock_acquire(a)		do { } while (0)
# define rcu_lock_release(a)		do { } while (0)

static inline int rcu_read_lock_held(void)
{
	return 1;
}

static inline int rcu_read_lock_bh_held(void)
{
	return 1;
}

static inline int rcu_read_lock_sched_held(void)
{
	return !preemptible();
}
#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_PROVE_RCU

/**
 * RCU_LOCKDEP_WARN - emit lockdep splat if specified condition is met
 * @c: condition to check
 * @s: informative message
 */
#define RCU_LOCKDEP_WARN(c, s)						\
	do {								\
		static bool __section(.data.unlikely) __warned;		\
		if (debug_lockdep_rcu_enabled() && !__warned && (c)) {	\
			__warned = true;				\
			lockdep_rcu_suspicious(__FILE__, __LINE__, s);	\
		}							\
	} while (0)

#if defined(CONFIG_PROVE_RCU) && !defined(CONFIG_PREEMPT_RCU)
static inline void rcu_preempt_sleep_check(void)
{
	RCU_LOCKDEP_WARN(lock_is_held(&rcu_lock_map),
			 "Illegal context switch in RCU read-side critical section");
}
#else /* #ifdef CONFIG_PROVE_RCU */
static inline void rcu_preempt_sleep_check(void) { }
#endif /* #else #ifdef CONFIG_PROVE_RCU */

#define rcu_sleep_check()						\
	do {								\
		rcu_preempt_sleep_check();				\
		RCU_LOCKDEP_WARN(lock_is_held(&rcu_bh_lock_map),	\
				 "Illegal context switch in RCU-bh read-side critical section"); \
		RCU_LOCKDEP_WARN(lock_is_held(&rcu_sched_lock_map),	\
				 "Illegal context switch in RCU-sched read-side critical section"); \
	} while (0)

#else /* #ifdef CONFIG_PROVE_RCU */

#define RCU_LOCKDEP_WARN(c, s) do { } while (0)
#define rcu_sleep_check() do { } while (0)

#endif /* #else #ifdef CONFIG_PROVE_RCU */

/*
 * Helper functions for rcu_dereference_check(), rcu_dereference_protected()
 * and rcu_assign_pointer().  Some of these could be folded into their
 * callers, but they are left separate in order to ease introduction of
 * multiple flavors of pointers to match the multiple flavors of RCU
 * (e.g., __rcu_bh, * __rcu_sched, and __srcu), should this make sense in
 * the future.
 */

#ifdef __CHECKER__
#define rcu_dereference_sparse(p, space) \
	((void)(((typeof(*p) space *)p) == p))
#else /* #ifdef __CHECKER__ */
#define rcu_dereference_sparse(p, space)
#endif /* #else #ifdef __CHECKER__ */

#define __rcu_access_pointer(p, space) \
({ \
	typeof(*p) *_________p1 = (typeof(*p) *__force)READ_ONCE(p); \
	rcu_dereference_sparse(p, space); \
	((typeof(*p) __force __kernel *)(_________p1)); \
})
#define __rcu_dereference_check(p, c, space) \
({ \
	/* Dependency order vs. p above. */ \
	typeof(*p) *________p1 = (typeof(*p) *__force)lockless_dereference(p); \
	RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_check() usage"); \
	rcu_dereference_sparse(p, space); \
	((typeof(*p) __force __kernel *)(________p1)); \
})
#define __rcu_dereference_protected(p, c, space) \
({ \
	RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_protected() usage"); \
	rcu_dereference_sparse(p, space); \
	((typeof(*p) __force __kernel *)(p)); \
})
#define rcu_dereference_raw(p) \
({ \
	/* Dependency order vs. p above. */ \
	typeof(p) ________p1 = lockless_dereference(p); \
	((typeof(*p) __force __kernel *)(________p1)); \
})

/**
 * RCU_INITIALIZER() - statically initialize an RCU-protected global variable
 * @v: The value to statically initialize with.
 */
#define RCU_INITIALIZER(v) (typeof(*(v)) __force __rcu *)(v)

/**
 * rcu_assign_pointer() - assign to RCU-protected pointer
 * @p: pointer to assign to
 * @v: value to assign (publish)
 *
 * Assigns the specified value to the specified RCU-protected
 * pointer, ensuring that any concurrent RCU readers will see
 * any prior initialization.
 *
 * Inserts memory barriers on architectures that require them
 * (which is most of them), and also prevents the compiler from
 * reordering the code that initializes the structure after the pointer
 * assignment.  More importantly, this call documents which pointers
 * will be dereferenced by RCU read-side code.
 *
 * In some special cases, you may use RCU_INIT_POINTER() instead
 * of rcu_assign_pointer().  RCU_INIT_POINTER() is a bit faster due
 * to the fact that it does not constrain either the CPU or the compiler.
 * That said, using RCU_INIT_POINTER() when you should have used
 * rcu_assign_pointer() is a very bad thing that results in
 * impossible-to-diagnose memory corruption.  So please be careful.
 * See the RCU_INIT_POINTER() comment header for details.
 *
 * Note that rcu_assign_pointer() evaluates each of its arguments only
 * once, appearances notwithstanding.  One of the "extra" evaluations
 * is in typeof() and the other visible only to sparse (__CHECKER__),
 * neither of which actually execute the argument.  As with most cpp
 * macros, this execute-arguments-only-once property is important, so
 * please be careful when making changes to rcu_assign_pointer() and the
 * other macros that it invokes.
 */
#define rcu_assign_pointer(p, v)					      \
({									      \
	uintptr_t _r_a_p__v = (uintptr_t)(v);				      \
									      \
	if (__builtin_constant_p(v) && (_r_a_p__v) == (uintptr_t)NULL)	      \
		WRITE_ONCE((p), (typeof(p))(_r_a_p__v));		      \
	else								      \
		smp_store_release(&p, RCU_INITIALIZER((typeof(p))_r_a_p__v)); \
	_r_a_p__v;							      \
})

/**
 * rcu_access_pointer() - fetch RCU pointer with no dereferencing
 * @p: The pointer to read
 *
 * Return the value of the specified RCU-protected pointer, but omit the
 * smp_read_barrier_depends() and keep the READ_ONCE().  This is useful
 * when the value of this pointer is accessed, but the pointer is not
 * dereferenced, for example, when testing an RCU-protected pointer against
 * NULL.  Although rcu_access_pointer() may also be used in cases where
 * update-side locks prevent the value of the pointer from changing, you
 * should instead use rcu_dereference_protected() for this use case.
 *
 * It is also permissible to use rcu_access_pointer() when read-side
 * access to the pointer was removed at least one grace period ago, as
 * is the case in the context of the RCU callback that is freeing up
 * the data, or after a synchronize_rcu() returns.  This can be useful
 * when tearing down multi-linked structures after a grace period
 * has elapsed.
 */
#define rcu_access_pointer(p) __rcu_access_pointer((p), __rcu)

/**
 * rcu_dereference_check() - rcu_dereference with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Do an rcu_dereference(), but check that the conditions under which the
 * dereference will take place are correct.  Typically the conditions
 * indicate the various locking conditions that should be held at that
 * point.  The check should return true if the conditions are satisfied.
 * An implicit check for being in an RCU read-side critical section
 * (rcu_read_lock()) is included.
 *
 * For example:
 *
 *	bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock));
 *
 * could be used to indicate to lockdep that foo->bar may only be dereferenced
 * if either rcu_read_lock() is held, or that the lock required to replace
 * the bar struct at foo->bar is held.
 *
 * Note that the list of conditions may also include indications of when a lock
 * need not be held, for example during initialisation or destruction of the
 * target struct:
 *
 *	bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock) ||
 *					      atomic_read(&foo->usage) == 0);
 *
 * Inserts memory barriers on architectures that require them
 * (currently only the Alpha), prevents the compiler from refetching
 * (and from merging fetches), and, more importantly, documents exactly
 * which pointers are protected by RCU and checks that the pointer is
 * annotated as __rcu.
 */
#define rcu_dereference_check(p, c) \
	__rcu_dereference_check((p), (c) || rcu_read_lock_held(), __rcu)

/**
 * rcu_dereference_bh_check() - rcu_dereference_bh with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the RCU-bh counterpart to rcu_dereference_check().
 */
#define rcu_dereference_bh_check(p, c) \
	__rcu_dereference_check((p), (c) || rcu_read_lock_bh_held(), __rcu)

/**
 * rcu_dereference_sched_check() - rcu_dereference_sched with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the RCU-sched counterpart to rcu_dereference_check().
 */
#define rcu_dereference_sched_check(p, c) \
	__rcu_dereference_check((p), (c) || rcu_read_lock_sched_held(), \
				__rcu)

/*
 * The tracing infrastructure traces RCU (we want that), but unfortunately
 * some of the RCU checks causes tracing to lock up the system.
 *
 * The no-tracing version of rcu_dereference_raw() must not call
 * rcu_read_lock_held().
 */
#define rcu_dereference_raw_notrace(p) __rcu_dereference_check((p), 1, __rcu)

/**
 * rcu_dereference_protected() - fetch RCU pointer when updates prevented
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Return the value of the specified RCU-protected pointer, but omit
 * both the smp_read_barrier_depends() and the READ_ONCE().  This
 * is useful in cases where update-side locks prevent the value of the
 * pointer from changing.  Please note that this primitive does *not*
 * prevent the compiler from repeating this reference or combining it
 * with other references, so it should not be used without protection
 * of appropriate locks.
 *
 * This function is only for update-side use.  Using this function
 * when protected only by rcu_read_lock() will result in infrequent
 * but very ugly failures.
 */
#define rcu_dereference_protected(p, c) \
	__rcu_dereference_protected((p), (c), __rcu)


/**
 * rcu_dereference() - fetch RCU-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * This is a simple wrapper around rcu_dereference_check().
 */
#define rcu_dereference(p) rcu_dereference_check(p, 0)

/**
 * rcu_dereference_bh() - fetch an RCU-bh-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
#define rcu_dereference_bh(p) rcu_dereference_bh_check(p, 0)

/**
 * rcu_dereference_sched() - fetch RCU-sched-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
#define rcu_dereference_sched(p) rcu_dereference_sched_check(p, 0)

/**
 * rcu_pointer_handoff() - Hand off a pointer from RCU to other mechanism
 * @p: The pointer to hand off
 *
 * This is simply an identity function, but it documents where a pointer
 * is handed off from RCU to some other synchronization mechanism, for
 * example, reference counting or locking.  In C11, it would map to
 * kill_dependency().  It could be used as follows:
 * ``
 *	rcu_read_lock();
 *	p = rcu_dereference(gp);
 *	long_lived = is_long_lived(p);
 *	if (long_lived) {
 *		if (!atomic_inc_not_zero(p->refcnt))
 *			long_lived = false;
 *		else
 *			p = rcu_pointer_handoff(p);
 *	}
 *	rcu_read_unlock();
 *``
 */
#define rcu_pointer_handoff(p) (p)

/**
 * rcu_read_lock() - mark the beginning of an RCU read-side critical section
 *
 * When synchronize_rcu() is invoked on one CPU while other CPUs
 * are within RCU read-side critical sections, then the
 * synchronize_rcu() is guaranteed to block until after all the other
 * CPUs exit their critical sections.  Similarly, if call_rcu() is invoked
 * on one CPU while other CPUs are within RCU read-side critical
 * sections, invocation of the corresponding RCU callback is deferred
 * until after the all the other CPUs exit their critical sections.
 *
 * Note, however, that RCU callbacks are permitted to run concurrently
 * with new RCU read-side critical sections.  One way that this can happen
 * is via the following sequence of events: (1) CPU 0 enters an RCU
 * read-side critical section, (2) CPU 1 invokes call_rcu() to register
 * an RCU callback, (3) CPU 0 exits the RCU read-side critical section,
 * (4) CPU 2 enters a RCU read-side critical section, (5) the RCU
 * callback is invoked.  This is legal, because the RCU read-side critical
 * section that was running concurrently with the call_rcu() (and which
 * therefore might be referencing something that the corresponding RCU
 * callback would free up) has completed before the corresponding
 * RCU callback is invoked.
 *
 * RCU read-side critical sections may be nested.  Any deferred actions
 * will be deferred until the outermost RCU read-side critical section
 * completes.
 *
 * You can avoid reading and understanding the next paragraph by
 * following this rule: don't put anything in an rcu_read_lock() RCU
 * read-side critical section that would block in a !PREEMPT kernel.
 * But if you want the full story, read on!
 *
 * In non-preemptible RCU implementations (TREE_RCU and TINY_RCU),
 * it is illegal to block while in an RCU read-side critical section.
 * In preemptible RCU implementations (PREEMPT_RCU) in CONFIG_PREEMPT
 * kernel builds, RCU read-side critical sections may be preempted,
 * but explicit blocking is illegal.  Finally, in preemptible RCU
 * implementations in real-time (with -rt patchset) kernel builds, RCU
 * read-side critical sections may be preempted and they may also block, but
 * only when acquiring spinlocks that are subject to priority inheritance.
 */
static __always_inline void rcu_read_lock(void)
{
	__rcu_read_lock();
	__acquire(RCU);
	rcu_lock_acquire(&rcu_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock() used illegally while idle");
}

/*
 * So where is rcu_write_lock()?  It does not exist, as there is no
 * way for writers to lock out RCU readers.  This is a feature, not
 * a bug -- this property is what provides RCU's performance benefits.
 * Of course, writers must coordinate with each other.  The normal
 * spinlock primitives work well for this, but any other technique may be
 * used as well.  RCU does not care how the writers keep out of each
 * others' way, as long as they do so.
 */

/**
 * rcu_read_unlock() - marks the end of an RCU read-side critical section.
 *
 * In most situations, rcu_read_unlock() is immune from deadlock.
 * However, in kernels built with CONFIG_RCU_BOOST, rcu_read_unlock()
 * is responsible for deboosting, which it does via rt_mutex_unlock().
 * Unfortunately, this function acquires the scheduler's runqueue and
 * priority-inheritance spinlocks.  This means that deadlock could result
 * if the caller of rcu_read_unlock() already holds one of these locks or
 * any lock that is ever acquired while holding them; or any lock which
 * can be taken from interrupt context because rcu_boost()->rt_mutex_lock()
 * does not disable irqs while taking ->wait_lock.
 *
 * That said, RCU readers are never priority boosted unless they were
 * preempted.  Therefore, one way to avoid deadlock is to make sure
 * that preemption never happens within any RCU read-side critical
 * section whose outermost rcu_read_unlock() is called with one of
 * rt_mutex_unlock()'s locks held.  Such preemption can be avoided in
 * a number of ways, for example, by invoking preempt_disable() before
 * critical section's outermost rcu_read_lock().
 *
 * Given that the set of locks acquired by rt_mutex_unlock() might change
 * at any time, a somewhat more future-proofed approach is to make sure
 * that that preemption never happens within any RCU read-side critical
 * section whose outermost rcu_read_unlock() is called with irqs disabled.
 * This approach relies on the fact that rt_mutex_unlock() currently only
 * acquires irq-disabled locks.
 *
 * The second of these two approaches is best in most situations,
 * however, the first approach can also be useful, at least to those
 * developers willing to keep abreast of the set of locks acquired by
 * rt_mutex_unlock().
 *
 * See rcu_read_lock() for more information.
 */
static inline void rcu_read_unlock(void)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock() used illegally while idle");
	__release(RCU);
	__rcu_read_unlock();
	rcu_lock_release(&rcu_lock_map); /* Keep acq info for rls diags. */
}

/**
 * rcu_read_lock_bh() - mark the beginning of an RCU-bh critical section
 *
 * This is equivalent of rcu_read_lock(), but to be used when updates
 * are being done using call_rcu_bh() or synchronize_rcu_bh(). Since
 * both call_rcu_bh() and synchronize_rcu_bh() consider completion of a
 * softirq handler to be a quiescent state, a process in RCU read-side
 * critical section must be protected by disabling softirqs. Read-side
 * critical sections in interrupt context can use just rcu_read_lock(),
 * though this should at least be commented to avoid confusing people
 * reading the code.
 *
 * Note that rcu_read_lock_bh() and the matching rcu_read_unlock_bh()
 * must occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock_bh() from one task if the matching rcu_read_lock_bh()
 * was invoked from some other task.
 */
static inline void rcu_read_lock_bh(void)
{
	local_bh_disable();
	__acquire(RCU_BH);
	rcu_lock_acquire(&rcu_bh_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock_bh() used illegally while idle");
}

/*
 * rcu_read_unlock_bh - marks the end of a softirq-only RCU critical section
 *
 * See rcu_read_lock_bh() for more information.
 */
static inline void rcu_read_unlock_bh(void)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock_bh() used illegally while idle");
	rcu_lock_release(&rcu_bh_lock_map);
	__release(RCU_BH);
	local_bh_enable();
}

/**
 * rcu_read_lock_sched() - mark the beginning of a RCU-sched critical section
 *
 * This is equivalent of rcu_read_lock(), but to be used when updates
 * are being done using call_rcu_sched() or synchronize_rcu_sched().
 * Read-side critical sections can also be introduced by anything that
 * disables preemption, including local_irq_disable() and friends.
 *
 * Note that rcu_read_lock_sched() and the matching rcu_read_unlock_sched()
 * must occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock_sched() from process context if the matching
 * rcu_read_lock_sched() was invoked from an NMI handler.
 */
static inline void rcu_read_lock_sched(void)
{
	preempt_disable();
	__acquire(RCU_SCHED);
	rcu_lock_acquire(&rcu_sched_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock_sched() used illegally while idle");
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
static inline notrace void rcu_read_lock_sched_notrace(void)
{
	preempt_disable_notrace();
	__acquire(RCU_SCHED);
}

/*
 * rcu_read_unlock_sched - marks the end of a RCU-classic critical section
 *
 * See rcu_read_lock_sched for more information.
 */
static inline void rcu_read_unlock_sched(void)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock_sched() used illegally while idle");
	rcu_lock_release(&rcu_sched_lock_map);
	__release(RCU_SCHED);
	preempt_enable();
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
static inline notrace void rcu_read_unlock_sched_notrace(void)
{
	__release(RCU_SCHED);
	preempt_enable_notrace();
}

/**
 * RCU_INIT_POINTER() - initialize an RCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * Initialize an RCU-protected pointer in special cases where readers
 * do not need ordering constraints on the CPU or the compiler.  These
 * special cases are:
 *
 * 1.	This use of RCU_INIT_POINTER() is NULLing out the pointer *or*
 * 2.	The caller has taken whatever steps are required to prevent
 *	RCU readers from concurrently accessing this pointer *or*
 * 3.	The referenced data structure has already been exposed to
 *	readers either at compile time or via rcu_assign_pointer() *and*
 *
 *	a.	You have not made *any* reader-visible changes to
 *		this structure since then *or*
 *	b.	It is OK for readers accessing this structure from its
 *		new location to see the old state of the structure.  (For
 *		example, the changes were to statistical counters or to
 *		other state where exact synchronization is not required.)
 *
 * Failure to follow these rules governing use of RCU_INIT_POINTER() will
 * result in impossible-to-diagnose memory corruption.  As in the structures
 * will look OK in crash dumps, but any concurrent RCU readers might
 * see pre-initialized values of the referenced data structure.  So
 * please be very careful how you use RCU_INIT_POINTER()!!!
 *
 * If you are creating an RCU-protected linked structure that is accessed
 * by a single external-to-structure RCU-protected pointer, then you may
 * use RCU_INIT_POINTER() to initialize the internal RCU-protected
 * pointers, but you must use rcu_assign_pointer() to initialize the
 * external-to-structure pointer *after* you have completely initialized
 * the reader-accessible portions of the linked structure.
 *
 * Note that unlike rcu_assign_pointer(), RCU_INIT_POINTER() provides no
 * ordering guarantees for either the CPU or the compiler.
 */
#define RCU_INIT_POINTER(p, v) \
	do { \
		rcu_dereference_sparse(p, __rcu); \
		WRITE_ONCE(p, RCU_INITIALIZER(v)); \
	} while (0)

/**
 * RCU_POINTER_INITIALIZER() - statically initialize an RCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * GCC-style initialization for an RCU-protected pointer in a structure field.
 */
#define RCU_POINTER_INITIALIZER(p, v) \
		.p = RCU_INITIALIZER(v)

/*
 * Does the specified offset indicate that the corresponding rcu_head
 * structure can be handled by kfree_rcu()?
 */
#define __is_kfree_rcu_offset(offset) ((offset) < 4096)

/*
 * Helper macro for kfree_rcu() to prevent argument-expansion eyestrain.
 */
#define __kfree_rcu(head, offset) \
	do { \
		BUILD_BUG_ON(!__is_kfree_rcu_offset(offset)); \
		kfree_call_rcu(head, (rcu_callback_t)(unsigned long)(offset)); \
	} while (0)

/**
 * kfree_rcu() - kfree an object after a grace period.
 * @ptr:	pointer to kfree
 * @rcu_head:	the name of the struct rcu_head within the type of @ptr.
 *
 * Many rcu callbacks functions just call kfree() on the base structure.
 * These functions are trivial, but their size adds up, and furthermore
 * when they are used in a kernel module, that module must invoke the
 * high-latency rcu_barrier() function at module-unload time.
 *
 * The kfree_rcu() function handles this issue.  Rather than encoding a
 * function address in the embedded rcu_head structure, kfree_rcu() instead
 * encodes the offset of the rcu_head structure within the base structure.
 * Because the functions are not allowed in the low-order 4096 bytes of
 * kernel virtual memory, offsets up to 4095 bytes can be accommodated.
 * If the offset is larger than 4095 bytes, a compile-time error will
 * be generated in __kfree_rcu().  If this error is triggered, you can
 * either fall back to use of call_rcu() or rearrange the structure to
 * position the rcu_head structure into the first 4096 bytes.
 *
 * Note that the allowable offset might decrease in the future, for example,
 * to allow something like kmem_cache_free_rcu().
 *
 * The BUILD_BUG_ON check must not involve any function calls, hence the
 * checks are done in macros here.
 */
#define kfree_rcu(ptr, rcu_head)					\
	__kfree_rcu(&((ptr)->rcu_head), offsetof(typeof(*(ptr)), rcu_head))


/*
 * Place this after a lock-acquisition primitive to guarantee that
 * an UNLOCK+LOCK pair acts as a full barrier.  This guarantee applies
 * if the UNLOCK and LOCK are executed by the same CPU or if the
 * UNLOCK and LOCK operate on the same lock variable.
 */
#ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE
#define smp_mb__after_unlock_lock()	smp_mb()  /* Full ordering for lock. */
#else /* #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */
#define smp_mb__after_unlock_lock()	do { } while (0)
#endif /* #else #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */


#endif /* __LINUX_RCUPDATE_H */
