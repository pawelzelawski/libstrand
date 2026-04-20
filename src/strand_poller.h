#ifndef STRAND_POLLER_H
#define STRAND_POLLER_H

/*
 * strand_poller.h - I/O poller internal interface.
 * epoll (Linux) and kqueue (OpenBSD). See ARCHITECTURE.md §5.
 */

#include "../include/strand.h"
#include "strand_internal.h"

#ifdef STRAND_LINUX
#include <sys/epoll.h>
#endif

#ifdef STRAND_OPENBSD
#include <sys/event.h>
#endif

/* fd sentinel values in the waiter table */
#define FD_ENTRY_EMPTY     (-1) /* slot never used or fully cleared */
#define FD_ENTRY_TOMBSTONE (-2) /* slot was removed; probe chain continues */

/*
 * fd_reg_state_t - Linux EPOLLONESHOT registration state per fd.
 *
 * NOT_REGISTERED - fd has never been registered, or was DEL'd.
 *                  Re-arm must use epoll_ctl ADD.
 * ACTIVE         - fd is currently registered and armed.
 * DISABLED       - fd fired; registration auto-disabled by EPOLLONESHOT.
 *                  Re-arm must use epoll_ctl MOD (not ADD - would get EEXIST).
 *
 * See ARCHITECTURE.md §5.2 and §5.4.
 */
typedef enum {
	FD_REG_NOT_REGISTERED = 0,
	FD_REG_ACTIVE         = 1,
	FD_REG_DISABLED       = 2,
} fd_reg_state_t;

/*
 * strand_fd_entry_t - one slot in the open-addressed fd waiter hash table.
 *
 * fd           - registered fd key; FD_ENTRY_EMPTY or FD_ENTRY_TOMBSTONE
 *                when the slot is not live.
 * read_waiter  - fiber parked in FIBER_PARKED_IO_READ on this fd, or NULL.
 * write_waiter - fiber parked in FIBER_PARKED_IO_WRITE on this fd, or NULL.
 * event_mask   - current registration interest mask (epoll/kqueue flags).
 *                Reflects the union of active read and write waiter interests.
 * reg_state    - Linux EPOLLONESHOT registration state.
 *                Ignored on OpenBSD (EV_DISPATCH has no equivalent tracking).
 * arm_token    - per-registration generation counter; bumped on each arm.
 *                Linux: packed with fd into epoll_event.data.u64 and
 *                validated on delivery to discard stale events.
 *                OpenBSD: entry pointer carried in kevent.udata.
 *                See ARCHITECTURE.md §5.3.
 * dbg_gen      - debug-only generation counter; incremented when fd reuse
 *                is detected (fd closed and reopened with same number).
 *                Used in debug builds to assert fd lifecycle correctness.
 *                See ARCHITECTURE.md §5.3 and CODING_STANDARDS.md §6.1.
 */
typedef struct strand_fd_entry {
	int              fd;
	strand_fiber_t  *read_waiter;
	strand_fiber_t  *write_waiter;
	uint32_t         event_mask;
	fd_reg_state_t   reg_state;
	uint32_t         arm_token;
#ifdef STRAND_DEBUG
	uint32_t         dbg_gen;
#endif
} strand_fd_entry_t;

/*
 * struct strand_poller - complete I/O poller.
 *
 * pollfd     - epoll fd (Linux) or kqueue fd (OpenBSD); -1 if not open.
 * table      - open-addressed fd waiter hash table; linear probing.
 * table_cap  - table capacity; always a power of 2.
 * table_len  - number of live entries (tombstones are not counted).
 *
 * The table grows (doubles) when load exceeds 75% (table_len * 4 >= table_cap * 3).
 * The hash function is: fd & (table_cap - 1).
 * Empty slots terminate probes; tombstones do not.
 *
 * See ARCHITECTURE.md §5.
 */
struct strand_poller {
	int                pollfd;
	strand_fd_entry_t *table;
	size_t             table_cap;
	size_t             table_len;
};

/* Default initial fd table capacity (power of 2). */
#define FD_TABLE_INITIAL_CAP ((size_t)64)

/*
 * POLLER_MAX_EVENTS - maximum number of events to retrieve in a single
 * epoll_wait / kevent call.  A batch of 64 balances per-call overhead
 * against the cost of processing many events per loop iteration.
 */
#define POLLER_MAX_EVENTS ((int)64)

/*
 * poller_create - allocate and initialise an I/O poller.
 * initial_cap - initial fd table capacity; rounded up to next power of 2.
 *               Pass 0 to use FD_TABLE_INITIAL_CAP.
 * Returns NULL on error (epoll_create1/kqueue or allocation failure).
 */
strand_poller_t *poller_create(size_t initial_cap);

/*
 * poller_destroy - close the poller fd and free all memory.
 * Debug builds assert that no active waiters remain in the table.
 * Safe to call with NULL.
 */
void poller_destroy(strand_poller_t *p);

/*
 * fd_table_lookup - find the entry for fd.
 * Returns a pointer to the entry, or NULL if fd is not registered.
 */
strand_fd_entry_t *fd_table_lookup(strand_poller_t *p, int fd);

/*
 * fd_table_insert - add a new entry for fd and return a pointer to it.
 * The caller must verify (via fd_table_lookup) that fd is not already
 * present before calling this function.
 * Grows the table if the load factor would exceed 75%.
 * Returns NULL on allocation failure.
 */
strand_fd_entry_t *fd_table_insert(strand_poller_t *p, int fd);

/*
 * fd_table_remove - remove the entry for fd from the table.
 * Must only be called when both read_waiter and write_waiter are NULL.
 * Debug builds assert this precondition.
 * No-op if fd is not found in the table.
 */
void fd_table_remove(strand_poller_t *p, int fd);

/*
 * poller_arm_fd - arm or re-arm fd in the OS poller.
 *
 * Linux: calls epoll_ctl ADD (if NOT_REGISTERED) or MOD (otherwise)
 *        with new_mask | EPOLLET | EPOLLONESHOT.
 *        Updates entry->event_mask, entry->reg_state, entry->arm_token.
 * OpenBSD: calls kevent with filter EV_ADD | EV_DISPATCH.
 *          filter is EVFILT_READ or EVFILT_WRITE; new_mask is ignored.
 *
 * On Linux, new_mask must already include EPOLLET | EPOLLONESHOT and
 * the correct combination of EPOLLIN / EPOLLOUT for all active waiters.
 *
 * entry->arm_token is incremented before the syscall so event data carries
 * the current registration epoch.
 *
 * Returns STRAND_OK or STRAND_ERR_IO on syscall failure.
 * See ARCHITECTURE.md §5.4 (Linux) and §5.7 (OpenBSD).
 */
int poller_arm_fd(strand_poller_t *p, int fd, uint32_t new_mask,
                  int filter, strand_fd_entry_t *e);

/*
 * fiber_io_wake - wake a fiber parked on an IO wait with a given result.
 *
 * Sets f->io_result = result, transitions f to FIBER_RUNNABLE, and
 * pushes f onto sched's run queue.  Called from poller_deliver_event
 * and poller_cancel_io.
 *
 * Must be called from the owning worker (same-worker path only).
 */
void fiber_io_wake(struct strand_scheduler *sched, strand_fiber_t *f,
                   int result);

/*
 * poller_poll - drain the OS polling fd and deliver all ready events.
 *
 * Calls epoll_wait (Linux) or kevent (OpenBSD) with the given timeout_ms,
 * then calls poller_deliver_event for every returned event.  Ready fibers
 * are pushed to sched's run queue by poller_deliver_event.
 *
 * timeout_ms:  -1 = block indefinitely until an event or wakeup;
 *               0 = return immediately (non-blocking);
 *              >0 = block for at most timeout_ms milliseconds.
 *
 * Called from strand_scheduler_advance Step 4 (timeout=0) and from the
 * strand_scheduler_run idle wait (blocking timeout).
 * See ARCHITECTURE.md §5.5, §5.6, §5.7.
 */
void poller_poll(struct strand_scheduler *sched, int timeout_ms);

/*
 * poller_cancel_io - cancel a fiber parked on an I/O wait (same-worker path).
 *
 * Removes the fiber's waiter from the fd table entry, adjusts the OS
 * registration, and wakes the fiber with STRAND_CANCELLED.
 *
 * Direction is inferred from f->state (FIBER_PARKED_IO_READ or IO_WRITE).
 * The fd is taken from f->parked_fd.
 *
 * Linux:
 *   - If the other direction still has a waiter: epoll_ctl MOD to the
 *     remaining direction only (EPOLLET | EPOLLONESHOT).
 *     No post-cancel readiness check is needed - the remaining waiter
 *     will wake on the next genuine edge or when the fd is re-armed.
 *   - If no other waiter: epoll_ctl DEL; reg_state -> NOT_REGISTERED.
 *
 * OpenBSD:
 *   - Delete the kevent filter for the cancelled direction (EV_DELETE).
 *   - EVFILT_READ and EVFILT_WRITE are independent; the other direction's
 *     filter, if any, remains registered without modification.
 *
 * The fd table entry is removed when both waiters are gone.
 *
 * Must be called from the owning worker (same-worker path only).
 * Cross-worker cancel is enqueued to the inject queue.
 * See ARCHITECTURE.md §5.8.
 */
void poller_cancel_io(struct strand_scheduler *sched, strand_fiber_t *f);

/*
 * poller_register_wakeup_fd - register the scheduler wakeup fd with the
 * poller's OS instance so that a write to the wakeup fd wakes a blocked
 * epoll_wait / kevent call.
 *
 * On Linux: adds the eventfd with EPOLLIN (persistent, no EPOLLONESHOT).
 * On OpenBSD: adds wakeup_pipe[0] with EVFILT_READ | EV_ADD (persistent,
 * no EV_DISPATCH).
 *
 * The registered event uses a dedicated sentinel token (Linux) / udata = NULL
 * (OpenBSD) so poller_deliver_event can identify it as the wakeup channel
 * and skip fiber wakeup.  The actual drain happens in advance Step 3.
 *
 * Returns STRAND_OK on success or STRAND_ERR_IO on syscall failure.
 * See ARCHITECTURE.md §5.
 */
int poller_register_wakeup_fd(strand_poller_t *p, int fd);

#endif /* STRAND_POLLER_H */
