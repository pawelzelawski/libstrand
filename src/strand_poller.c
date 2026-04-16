/*
 * strand_poller.c — I/O multiplexing: epoll (Linux) and kqueue (OpenBSD).
 * See ARCHITECTURE.md §5.
 *
 * Task 4.1: strand_poller_t and fd waiter hash table implementation.
 * Task 4.2: strand_fiber_wait_readable / strand_fiber_wait_writable.
 * Task 4.3: poller_poll and event delivery.
 * Task 4.5: wakeup fd integration into poller.
 */

#include "strand_poller.h"
#include "strand_context.h"
#include "strand_sched.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef STRAND_LINUX
#include <sys/epoll.h>
#endif

#ifdef STRAND_OPENBSD
#include <sys/event.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------
 */

/*
 * next_pow2 — round n up to the next power of 2.
 * Returns n unchanged if n is already a power of 2.
 * n must be > 0.
 */
static size_t
next_pow2(size_t n)
{
	size_t p = 1;

	while (p < n)
		p <<= 1;
	return p;
}

/*
 * fd_hash — primary slot index for fd in a table of cap slots.
 * cap must be a power of 2.
 */
static size_t
fd_hash(int fd, size_t cap)
{
	return (size_t)(unsigned int)fd & (cap - 1);
}

/*
 * fd_table_grow — double the table capacity and rehash all live entries.
 * Tombstones are dropped on rehash (they are no longer needed in the new
 * table because all live entries are inserted into empty slots).
 * Returns STRAND_OK on success, STRAND_ERR_NOMEM on allocation failure.
 */
static int
fd_table_grow(strand_poller_t *p)
{
	size_t             new_cap, i, slot;
	strand_fd_entry_t *new_table, *e;

	new_cap   = p->table_cap * 2;
	new_table = calloc(new_cap, sizeof(*new_table));
	if (new_table == NULL)
		return STRAND_ERR_NOMEM;

	/* Mark all new slots as empty. */
	for (i = 0; i < new_cap; i++)
		new_table[i].fd = FD_ENTRY_EMPTY;

	/* Rehash only live entries; tombstones are discarded. */
	for (i = 0; i < p->table_cap; i++) {
		e = &p->table[i];
		if (e->fd < 0) /* empty or tombstone */
			continue;
		slot = fd_hash(e->fd, new_cap);
		while (new_table[slot].fd != FD_ENTRY_EMPTY)
			slot = (slot + 1) & (new_cap - 1);
		new_table[slot] = *e;
	}

	free(p->table);
	p->table     = new_table;
	p->table_cap = new_cap;
	return STRAND_OK;
}

/* ---------------------------------------------------------------------------
 * poller_create / poller_destroy
 * ---------------------------------------------------------------------------
 */

/*
 * poller_create — allocate and initialise an I/O poller.
 *
 * Creates the OS polling fd (epoll on Linux, kqueue on OpenBSD) and
 * allocates the open-addressed fd waiter hash table.
 *
 * initial_cap — desired initial table capacity; rounded up to the next
 *               power of 2.  Pass 0 to use FD_TABLE_INITIAL_CAP.
 * Returns NULL on any error.
 */
strand_poller_t *
poller_create(size_t initial_cap)
{
	strand_poller_t   *p;
	size_t             cap, i;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	cap = (initial_cap == 0) ? FD_TABLE_INITIAL_CAP : next_pow2(initial_cap);

	p->table = calloc(cap, sizeof(*p->table));
	if (p->table == NULL) {
		free(p);
		return NULL;
	}

	/* Initialise every slot to empty. */
	for (i = 0; i < cap; i++)
		p->table[i].fd = FD_ENTRY_EMPTY;

	p->table_cap = cap;
	p->table_len = 0;
	p->pollfd    = -1;

#ifdef STRAND_LINUX
	/*
	 * EPOLL_CLOEXEC: close the epoll fd across exec so child processes
	 * do not inherit the poller.  See ARCHITECTURE.md §5.
	 */
	p->pollfd = epoll_create1(EPOLL_CLOEXEC);
	if (p->pollfd == -1) {
		free(p->table);
		free(p);
		return NULL;
	}
#endif

#ifdef STRAND_OPENBSD
	p->pollfd = kqueue();
	if (p->pollfd == -1) {
		free(p->table);
		free(p);
		return NULL;
	}
#endif

	return p;
}

/*
 * poller_destroy — close the polling fd and free all memory.
 *
 * Debug builds assert that no active read or write waiters remain in the
 * table.  It is a programming error to destroy the poller while fibers are
 * still parked on fds.
 * Safe to call with NULL.
 */
void
poller_destroy(strand_poller_t *p)
{
	if (p == NULL)
		return;

#ifdef STRAND_DEBUG
	{
		size_t             i;
		strand_fd_entry_t *e;

		for (i = 0; i < p->table_cap; i++) {
			e = &p->table[i];
			if (e->fd < 0)
				continue;
			STRAND_DEBUG_ASSERT(e->read_waiter == NULL);
			STRAND_DEBUG_ASSERT(e->write_waiter == NULL);
		}
	}
#endif

	if (p->pollfd != -1)
		close(p->pollfd);

	free(p->table);
	free(p);
}

/* ---------------------------------------------------------------------------
 * fd waiter hash table operations
 * ---------------------------------------------------------------------------
 */

/*
 * fd_table_lookup — find the entry for fd using linear probing.
 *
 * Probe sequence: hash(fd), hash(fd)+1, ..., wrapping at table_cap.
 * An FD_ENTRY_EMPTY slot terminates the probe (fd cannot be past it).
 * An FD_ENTRY_TOMBSTONE slot does NOT terminate the probe (fd may be further
 * along the chain).
 *
 * Returns a pointer to the entry, or NULL if fd is not in the table.
 */
strand_fd_entry_t *
fd_table_lookup(strand_poller_t *p, int fd)
{
	size_t             slot, i;
	strand_fd_entry_t *e;

	slot = fd_hash(fd, p->table_cap);
	for (i = 0; i < p->table_cap; i++) {
		e = &p->table[(slot + i) & (p->table_cap - 1)];
		if (e->fd == FD_ENTRY_EMPTY)
			return NULL;
		if (e->fd == fd)
			return e;
		/* FD_ENTRY_TOMBSTONE: continue probing */
	}
	return NULL;
}

/*
 * fd_table_insert — add a new entry for fd and return a pointer to it.
 *
 * The caller must have verified via fd_table_lookup that fd is not already
 * in the table.
 *
 * Grows the table before inserting if the load factor would exceed 75%
 * (table_len * 4 >= table_cap * 3).  When a tombstone slot is encountered
 * before the first empty slot, the tombstone is reused for the new entry so
 * that probe chains do not grow without bound.
 *
 * Returns a pointer to the initialised entry, or NULL on allocation failure.
 */
strand_fd_entry_t *
fd_table_insert(strand_poller_t *p, int fd)
{
	size_t             slot, i;
	strand_fd_entry_t *e, *tombstone;

	/* Grow before inserting if load would exceed 75%. */
	if (p->table_len * 4 >= p->table_cap * 3) {
		if (fd_table_grow(p) != STRAND_OK)
			return NULL;
	}

	tombstone = NULL;
	slot      = fd_hash(fd, p->table_cap);

	for (i = 0; i < p->table_cap; i++) {
		e = &p->table[(slot + i) & (p->table_cap - 1)];

		if (e->fd == FD_ENTRY_EMPTY) {
			/*
			 * Prefer the earliest tombstone seen so that reuse
			 * keeps entries close to their home slot.
			 */
			if (tombstone != NULL)
				e = tombstone;
			e->fd           = fd;
			e->read_waiter  = NULL;
			e->write_waiter = NULL;
			e->event_mask   = 0;
			e->reg_state    = FD_REG_NOT_REGISTERED;
			e->arm_token    = 0;
#ifdef STRAND_DEBUG
			e->dbg_gen = 0;
#endif
			p->table_len++;
			return e;
		}

		if (e->fd == FD_ENTRY_TOMBSTONE && tombstone == NULL)
			tombstone = e;
	}

	/*
	 * Table entirely full — should never happen after the 75% load-factor
	 * check above, but guard against it defensively.
	 */
	return NULL;
}

/*
 * fd_table_remove — remove the entry for fd from the table.
 * ...existing code...
 */
void
fd_table_remove(strand_poller_t *p, int fd)
{
	strand_fd_entry_t *e;

	e = fd_table_lookup(p, fd);
	if (e == NULL)
		return;

	/*
	 * SAFETY: removing an fd entry while a waiter is still registered
	 * would leave a dangling fiber pointer.  Both waiters must be NULL
	 * before the entry can be removed.
	 * See ARCHITECTURE.md §5.2.
	 */
	STRAND_DEBUG_ASSERT(e->read_waiter == NULL);
	STRAND_DEBUG_ASSERT(e->write_waiter == NULL);

	e->fd           = FD_ENTRY_TOMBSTONE;
	e->read_waiter  = NULL;
	e->write_waiter = NULL;
	e->event_mask   = 0;
	e->reg_state    = FD_REG_NOT_REGISTERED;
	e->arm_token    = 0;
#ifdef STRAND_DEBUG
	e->dbg_gen = 0;
#endif
	p->table_len--;
}

/* ---------------------------------------------------------------------------
 * poller_arm_fd — arm or re-arm an fd in the OS poller.
 *
 * Linux: issues epoll_ctl ADD (NOT_REGISTERED) or MOD (otherwise) with
 *        new_mask (caller is responsible for including EPOLLET | EPOLLONESHOT
 *        and the correct EPOLLIN / EPOLLOUT combination).
 *        Stores entry pointer in ev.data.ptr as the per-registration token
 *        so that poller_deliver_event can find the entry without a hash lookup.
 *        Increments arm_token before the syscall; updates event_mask and
 *        reg_state on success.
 *
 * OpenBSD: issues kevent EV_ADD | EV_DISPATCH for the given filter
 *          (EVFILT_READ or EVFILT_WRITE).  new_mask is not used on OpenBSD.
 *          Stores entry pointer in kev.udata.
 *
 * Returns STRAND_OK on success or STRAND_ERR_IO on syscall failure.
 * See ARCHITECTURE.md §5.4 and §5.7.
 * ---------------------------------------------------------------------------
 */
int
poller_arm_fd(strand_poller_t *p, int fd, uint32_t new_mask,
              int filter, strand_fd_entry_t *e)
{
#ifdef STRAND_LINUX
	struct epoll_event ev;
	int                op;

	op = (e->reg_state == FD_REG_NOT_REGISTERED) ? EPOLL_CTL_ADD
	                                              : EPOLL_CTL_MOD;
	ev.events   = new_mask;
	ev.data.ptr = e;

	/*
	 * Increment arm_token before the syscall.  The token is stored in
	 * ev.data.ptr (entry pointer) and implicitly in entry->arm_token.
	 * poller_deliver_event can use arm_token to detect stale events after
	 * a cancel + re-register sequence.
	 * See ARCHITECTURE.md §5.3.
	 */
	e->arm_token++;

	if (epoll_ctl(p->pollfd, op, fd, &ev) == -1)
		return (STRAND_ERR_IO);

	e->event_mask = new_mask;
	e->reg_state  = FD_REG_ACTIVE;
	return (STRAND_OK);
#endif

#ifdef STRAND_OPENBSD
	struct kevent kev;

	(void)new_mask; /* mask is implicit in the filter on OpenBSD */

	/*
	 * EV_ONESHOT is used instead of EV_DISPATCH so that each wait
	 * registration is a brand-new filter install.  EV_DISPATCH only
	 * disables the filter after delivery; re-enabling it with EV_ADD on
	 * an fd where the level condition is already satisfied does not
	 * immediately re-queue the knote on OpenBSD.  EV_ONESHOT deletes the
	 * filter after delivery, so the next EV_ADD registers a genuinely new
	 * filter — the kernel checks the current level and queues the event
	 * immediately if the condition is already met.
	 * See ARCHITECTURE.md §5.7.
	 */
	EV_SET(&kev, (uintptr_t)fd, filter, EV_ADD | EV_ONESHOT, 0, 0, e);
	e->arm_token++;

	if (kevent(p->pollfd, &kev, 1, NULL, 0, NULL) == -1)
		return (STRAND_ERR_IO);

	return (STRAND_OK);
#endif
}

/* ---------------------------------------------------------------------------
 * fiber_io_wake — transition a parked IO fiber to RUNNABLE.
 *
 * Sets io_result on the fiber, transitions state to FIBER_RUNNABLE, and
 * appends the fiber to the scheduler run queue.  Called from
 * poller_deliver_event (Task 4.3) and poller_cancel_io (Task 4.4).
 *
 * Must be called from the owning worker.
 * ---------------------------------------------------------------------------
 */
void
fiber_io_wake(struct strand_scheduler *sched, strand_fiber_t *f, int result)
{
	f->io_result = result;
	atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);
	run_queue_push(sched, f);
}

/* ---------------------------------------------------------------------------
 * fiber_wait_io — common implementation for wait_readable / wait_writable.
 *
 * dir: 0 = read (FIBER_PARKED_IO_READ), 1 = write (FIBER_PARKED_IO_WRITE).
 *
 * Steps:
 *   1. Assert fiber context; debug-assert O_NONBLOCK on fd.
 *   2. Lookup or insert fd entry in poller table.
 *   3. Reject duplicate waiter in same direction (STRAND_ERR_IO_CONFLICT).
 *   4. Compute new event mask (Linux) or select filter (OpenBSD).
 *   5. Arm fd via poller_arm_fd.
 *   6. Store waiter pointer; record parked_fd on fiber.
 *   7. Transition FIBER_RUNNING -> FIBER_PARKED_IO_READ or IO_WRITE.
 *   8. strand_context_switch back to scheduler.
 *   9. Return f->io_result (set by waker before run_queue_push).
 *
 * See ARCHITECTURE.md §5.2, §5.3, §5.4, §5.7.
 * ---------------------------------------------------------------------------
 */
static int
fiber_wait_io(strand_scheduler_t *sched, int fd, int dir)
{
	strand_fiber_t    *f;
	strand_fd_entry_t *e;
	int                inserted, rc;

	STRAND_DEBUG_ASSERT(sched != NULL);
	STRAND_DEBUG_ASSERT(sched->current_fiber != NULL);

	f = sched->current_fiber;

	/*
	 * SAFETY: O_NONBLOCK must be set on fd before calling wait_readable /
	 * wait_writable.  A blocking fd would stall the entire worker thread.
	 * See ARCHITECTURE.md §5.1 and CODING_STANDARDS.md §6.1.
	 */
#ifdef STRAND_DEBUG
	{
		int fl = fcntl(fd, F_GETFL);
		STRAND_DEBUG_ASSERT(fl != -1 && (fl & O_NONBLOCK) &&
		    "fd passed to strand_fiber_wait without O_NONBLOCK");
	}
#endif

	inserted = 0;
	e = fd_table_lookup(sched->poller, fd);
	if (e == NULL) {
		e = fd_table_insert(sched->poller, fd);
		if (e == NULL)
			return (STRAND_ERR_NOMEM);
		inserted = 1;
	}

	/*
	 * Reject duplicate waiter in the requested direction.
	 * Multiple waiters in the same direction are forbidden — see
	 * ARCHITECTURE.md §5.3.
	 */
	if (dir == 0 && e->read_waiter != NULL)
		return (STRAND_ERR_IO_CONFLICT);
	if (dir == 1 && e->write_waiter != NULL)
		return (STRAND_ERR_IO_CONFLICT);

	/*
	 * Compute event mask (Linux) or filter (OpenBSD) and arm the fd.
	 *
	 * Linux: mask reflects all active directions after this registration.
	 *   Both EPOLLIN and EPOLLOUT may be set if both directions have waiters.
	 *   EPOLLET | EPOLLONESHOT are always set.  See ARCHITECTURE.md §5.4.
	 *
	 * OpenBSD: EVFILT_READ and EVFILT_WRITE are independent filters.
	 *   Each direction uses a separate kevent call.  See ARCHITECTURE.md §5.7.
	 */
#ifdef STRAND_LINUX
	{
		uint32_t new_mask;

		new_mask = EPOLLET | EPOLLONESHOT;
		/* Add EPOLLIN if registering read or if write waiter already active. */
		if (dir == 0 || e->read_waiter != NULL)
			new_mask |= EPOLLIN;
		/* Add EPOLLOUT if registering write or if read waiter already active. */
		if (dir == 1 || e->write_waiter != NULL)
			new_mask |= EPOLLOUT;
		rc = poller_arm_fd(sched->poller, fd, new_mask, 0, e);
	}
#endif

#ifdef STRAND_OPENBSD
	{
		int filter = (dir == 0) ? EVFILT_READ : EVFILT_WRITE;
		rc = poller_arm_fd(sched->poller, fd, 0, filter, e);
	}
#endif

	if (rc != STRAND_OK) {
		/*
		 * Arm failed.  If we just inserted this entry and it has no
		 * other waiters, remove it to keep the table clean.
		 */
		if (inserted && e->read_waiter == NULL && e->write_waiter == NULL)
			fd_table_remove(sched->poller, fd);
		return (rc);
	}

	/* Store the waiter pointer in the entry. */
	if (dir == 0)
		e->read_waiter = f;
	else
		e->write_waiter = f;

	/* Record the fd on the fiber so the cancel path can find the entry. */
	f->parked_fd = fd;

	/*
	 * Transition FIBER_RUNNING -> FIBER_PARKED_IO_READ / IO_WRITE.
	 * Relaxed ordering: visibility is provided by the context switch below.
	 */
	atomic_store_explicit(&f->state,
	    (dir == 0) ? FIBER_PARKED_IO_READ : FIBER_PARKED_IO_WRITE,
	    memory_order_relaxed);

	/* Switch back to the scheduler.  Resumes here when the fd fires,
	 * the fiber is cancelled, or the fd enters an error state. */
	strand_context_switch(f, &sched->scheduler_ctx);

	/* Return the result set by the waker (STRAND_OK, STRAND_CANCELLED,
	 * or STRAND_ERR_IO).  See fiber_io_wake and ARCHITECTURE.md §5.3. */
	return (f->io_result);
}

/* ---------------------------------------------------------------------------
 * poller_deliver_event — process one event returned by epoll_wait / kevent.
 *
 * Linux path:
 *   - NULL data.ptr: wakeup fd sentinel; skip fiber delivery.
 *   - EPOLLERR / EPOLLHUP: wake all waiters on the fd with STRAND_ERR_IO.
 *   - EPOLLIN: wake read waiter with STRAND_OK.
 *   - EPOLLOUT: wake write waiter with STRAND_OK.
 *   - If one direction woke and the other still has a waiter: re-arm the
 *     remaining direction with MOD, then immediately call epoll_wait with
 *     timeout=0 and process ALL returned events (post-re-arm check).
 *   - Remove the fd table entry when both waiters are gone.
 *
 * OpenBSD path:
 *   - NULL udata: wakeup fd sentinel; skip fiber delivery.
 *   - EV_EOF: wake the waiter for this filter with STRAND_ERR_IO.
 *   - Otherwise: wake the waiter for this filter with STRAND_OK.
 *   - No post-re-arm check needed (EV_DISPATCH is level-triggered).
 *
 * See ARCHITECTURE.md §5.5, §5.6, §5.7.
 * ---------------------------------------------------------------------------
 */

#ifdef STRAND_LINUX

static void
poller_deliver_event(strand_scheduler_t *sched, struct epoll_event *ev)
{
	strand_fd_entry_t *e;
	strand_poller_t   *p = sched->poller;
	uint32_t           flags;
	int                fd;
	int                woke_read = 0, woke_write = 0;

	/* NULL sentinel: wakeup fd event; no fiber to wake. */
	if (ev->data.ptr == NULL)
		return;

	e     = (strand_fd_entry_t *)ev->data.ptr;
	flags = ev->events;
	fd    = e->fd;

	/*
	 * SAFETY: EPOLLERR and EPOLLHUP are delivered by the kernel
	 * unconditionally, regardless of the registered interest mask.
	 * Any fiber waiting on this fd in any direction must be woken
	 * with an error result — leaving a fiber parked on an fd in an
	 * error or hangup state would result in it parking forever.
	 * These flags must be checked before EPOLLIN/EPOLLOUT.
	 * See ARCHITECTURE.md §5.6.
	 */
	if (flags & (EPOLLERR | EPOLLHUP)) {
		e->reg_state = FD_REG_DISABLED;
		if (e->read_waiter != NULL) {
			strand_fiber_t *f = e->read_waiter;
			e->read_waiter = NULL;
			fiber_io_wake(sched, f, STRAND_ERR_IO);
		}
		if (e->write_waiter != NULL) {
			strand_fiber_t *f = e->write_waiter;
			e->write_waiter = NULL;
			fiber_io_wake(sched, f, STRAND_ERR_IO);
		}
		if (e->read_waiter == NULL && e->write_waiter == NULL)
			fd_table_remove(p, fd);
		return;
	}

	/*
	 * EPOLLONESHOT auto-disabled the registration when the event fired.
	 * Update reg_state to DISABLED so the next arm uses MOD, not ADD.
	 */
	e->reg_state = FD_REG_DISABLED;

	if ((flags & EPOLLIN) && e->read_waiter != NULL) {
		strand_fiber_t *f = e->read_waiter;
		e->read_waiter = NULL;
		fiber_io_wake(sched, f, STRAND_OK);
		woke_read = 1;
	}
	if ((flags & EPOLLOUT) && e->write_waiter != NULL) {
		strand_fiber_t *f = e->write_waiter;
		e->write_waiter = NULL;
		fiber_io_wake(sched, f, STRAND_OK);
		woke_write = 1;
	}

	/*
	 * If one direction woke and the other still has a waiter, re-arm the
	 * remaining direction.  EPOLLET only fires on state transitions — if
	 * the remaining direction was already ready before the MOD call, no
	 * new edge will be generated and the waiter would park forever.
	 * A zero-timeout epoll_wait immediately after the MOD call detects
	 * this case.
	 */
	if ((woke_read || woke_write) &&
	    (e->read_waiter != NULL || e->write_waiter != NULL)) {
		uint32_t new_mask = EPOLLET | EPOLLONESHOT;
		if (e->read_waiter  != NULL) new_mask |= EPOLLIN;
		if (e->write_waiter != NULL) new_mask |= EPOLLOUT;

		if (poller_arm_fd(p, fd, new_mask, 0, e) == STRAND_OK) {
			struct epoll_event check_evs[POLLER_MAX_EVENTS];
			int nfds, i;

			/*
			 * SAFETY: After re-arming with EPOLLONESHOT, the
			 * remaining direction may already be ready (the fd was
			 * ready before the MOD call and no new edge occurred).
			 * A zero-timeout epoll_wait is required to detect this.
			 * ALL events returned by this zero-timeout poll must be
			 * processed — EPOLLONESHOT has consumed them from the
			 * kernel queue and they will not reappear in any
			 * subsequent poll, regardless of which fd they belong to.
			 * See ARCHITECTURE.md §5.5.
			 */
			nfds = epoll_wait(p->pollfd, check_evs,
			                  POLLER_MAX_EVENTS, 0);
			for (i = 0; i < nfds; i++)
				poller_deliver_event(sched, &check_evs[i]);
		} else {
			/*
			 * Re-arm failed.  Wake the remaining waiter with an
			 * error rather than leaving it parked on a broken fd.
			 */
			if (e->read_waiter != NULL) {
				strand_fiber_t *f = e->read_waiter;
				e->read_waiter = NULL;
				fiber_io_wake(sched, f, STRAND_ERR_IO);
			}
			if (e->write_waiter != NULL) {
				strand_fiber_t *f = e->write_waiter;
				e->write_waiter = NULL;
				fiber_io_wake(sched, f, STRAND_ERR_IO);
			}
		}
	}

	/*
	 * Remove the table entry if both waiters are gone.  The recursive
	 * zero-timeout delivery above may have already tombstoned this entry;
	 * fd_table_remove is a no-op in that case (lookup returns NULL).
	 */
	if (e->read_waiter == NULL && e->write_waiter == NULL)
		fd_table_remove(p, fd);
}

#endif /* STRAND_LINUX */

#ifdef STRAND_OPENBSD

static void
poller_deliver_event(strand_scheduler_t *sched, struct kevent *kev)
{
	strand_fd_entry_t *e;
	strand_poller_t   *p = sched->poller;
	int                fd;
	int                result;

	/* NULL sentinel: wakeup fd event; no fiber to wake. */
	if (kev->udata == NULL)
		return;

	e  = (strand_fd_entry_t *)kev->udata;
	fd = e->fd;

	/*
	 * EV_EOF: the remote peer closed the connection, or the write end of
	 * a pipe was closed.  Wake the waiter for this filter with an error
	 * result so it is not left parked on a half-closed fd.
	 * See ARCHITECTURE.md §5.7.
	 */
	result = (kev->flags & EV_EOF) ? STRAND_ERR_IO : STRAND_OK;

	if (kev->filter == EVFILT_READ && e->read_waiter != NULL) {
		strand_fiber_t *f = e->read_waiter;
		e->read_waiter = NULL;
		fiber_io_wake(sched, f, result);
	} else if (kev->filter == EVFILT_WRITE && e->write_waiter != NULL) {
		strand_fiber_t *f = e->write_waiter;
		e->write_waiter = NULL;
		fiber_io_wake(sched, f, result);
	}

	/* Remove the table entry when both waiters are gone. */
	if (e->read_waiter == NULL && e->write_waiter == NULL)
		fd_table_remove(p, fd);
}

#endif /* STRAND_OPENBSD */

/* ---------------------------------------------------------------------------
 * poller_poll — drain the OS polling fd and deliver all ready events.
 *
 * Calls epoll_wait (Linux) or kevent (OpenBSD) with the given timeout_ms.
 * Each returned event is passed to poller_deliver_event, which wakes any
 * parked fiber and pushes it onto the scheduler run queue.
 *
 * timeout_ms: -1 = block indefinitely; 0 = non-blocking; >0 = bounded wait.
 * See ARCHITECTURE.md §5.5, §5.6, §5.7.
 * ---------------------------------------------------------------------------
 */
void
poller_poll(strand_scheduler_t *sched, int timeout_ms)
{
	strand_poller_t *p = sched->poller;

#ifdef STRAND_LINUX
	struct epoll_event evs[POLLER_MAX_EVENTS];
	int                nfds, i;

	nfds = epoll_wait(p->pollfd, evs, POLLER_MAX_EVENTS, timeout_ms);
	for (i = 0; i < nfds; i++)
		poller_deliver_event(sched, &evs[i]);
#endif

#ifdef STRAND_OPENBSD
	struct kevent    evs[POLLER_MAX_EVENTS];
	struct timespec  ts, *tsp;
	int              nfds, i;

	if (timeout_ms < 0) {
		tsp = NULL; /* block indefinitely */
	} else {
		ts.tv_sec  = timeout_ms / 1000;
		ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
		tsp = &ts;
	}
	nfds = kevent(p->pollfd, NULL, 0, evs, POLLER_MAX_EVENTS, tsp);
	for (i = 0; i < nfds; i++)
		poller_deliver_event(sched, &evs[i]);
#endif
}

/* ---------------------------------------------------------------------------
 * poller_cancel_io — cancel a fiber parked on an I/O wait (same-worker).
 *
 * Infers direction from f->state, clears the waiter from the entry, updates
 * the OS registration, removes the entry if empty, and wakes f with
 * STRAND_CANCELLED.  See ARCHITECTURE.md §5.8.
 * ---------------------------------------------------------------------------
 */
void
poller_cancel_io(struct strand_scheduler *sched, strand_fiber_t *f)
{
	strand_poller_t   *p   = sched->poller;
	int                fd  = f->parked_fd;
	fiber_state_t      st  = atomic_load_explicit(&f->state,
	                             memory_order_relaxed);
	int                dir = (st == FIBER_PARKED_IO_READ) ? 0 : 1;
	strand_fd_entry_t *e;

	e = fd_table_lookup(p, fd);

	/*
	 * SAFETY: the fd table entry must exist while any fiber is parked on
	 * the fd.  A missing entry indicates a lifecycle bug — the entry was
	 * removed while a waiter was still registered.
	 * See ARCHITECTURE.md §5.2.
	 */
	STRAND_DEBUG_ASSERT(e != NULL);
	if (e == NULL) {
		/* Release build: wake with error rather than leaving f parked. */
		fiber_io_wake(sched, f, STRAND_ERR_IO);
		return;
	}

	/* Clear the cancelled direction's waiter pointer. */
	if (dir == 0)
		e->read_waiter = NULL;
	else
		e->write_waiter = NULL;

#ifdef STRAND_LINUX
	if (e->read_waiter != NULL || e->write_waiter != NULL) {
		/*
		 * Other direction still has a waiter: MOD the registration to
		 * cover only the remaining direction.  No post-cancel readiness
		 * check is needed — the remaining waiter will wake on the next
		 * genuine edge.  See ARCHITECTURE.md §5.8.
		 */
		uint32_t new_mask = EPOLLET | EPOLLONESHOT;
		if (e->read_waiter  != NULL) new_mask |= EPOLLIN;
		if (e->write_waiter != NULL) new_mask |= EPOLLOUT;
		if (e->reg_state != FD_REG_NOT_REGISTERED)
			(void)poller_arm_fd(p, fd, new_mask, 0, e);
	} else {
		/*
		 * No remaining waiters: remove the fd from epoll entirely.
		 * Errors are ignored — the fd may have been closed concurrently
		 * by the application (same-worker path only here; cross-worker
		 * close races are a Phase 5 concern).
		 */
		if (e->reg_state != FD_REG_NOT_REGISTERED) {
			(void)epoll_ctl(p->pollfd, EPOLL_CTL_DEL, fd, NULL);
			e->reg_state = FD_REG_NOT_REGISTERED;
		}
	}
#endif /* STRAND_LINUX */

#ifdef STRAND_OPENBSD
	{
		struct kevent kev;
		int cancel_filter = (dir == 0) ? EVFILT_READ : EVFILT_WRITE;

		/*
		 * Delete the filter for the cancelled direction.
		 * EVFILT_READ and EVFILT_WRITE are independent; the other
		 * direction's filter, if registered, remains active.
		 * Errors are ignored — the filter may already be gone if the
		 * fd was closed by the application.
		 * See ARCHITECTURE.md §5.7 and §5.8.
		 */
		EV_SET(&kev, (uintptr_t)fd, cancel_filter, EV_DELETE,
		       0, 0, NULL);
		(void)kevent(p->pollfd, &kev, 1, NULL, 0, NULL);
	}
#endif /* STRAND_OPENBSD */

	/* Remove the table entry when both waiters are gone. */
	if (e->read_waiter == NULL && e->write_waiter == NULL)
		fd_table_remove(p, fd);

	/* Wake the cancelled fiber with STRAND_CANCELLED. */
	fiber_io_wake(sched, f, STRAND_CANCELLED);
}

/* ---------------------------------------------------------------------------
 * poller_register_wakeup_fd — register the scheduler wakeup fd with the
 * poller OS instance so that a write to the wakeup channel interrupts a
 * blocked epoll_wait / kevent call.
 *
 * The event is registered WITHOUT EPOLLONESHOT / EV_DISPATCH (persistent)
 * so that successive stop signals all unblock the wait.
 * data.ptr = NULL (Linux) / udata = NULL (OpenBSD) serves as the delivery
 * sentinel — poller_deliver_event skips entries with a NULL pointer.
 *
 * Returns STRAND_OK on success or STRAND_ERR_IO on syscall failure.
 * See ARCHITECTURE.md §5 and DEVELOPMENT.md Task 4.5.
 * ---------------------------------------------------------------------------
 */
int
poller_register_wakeup_fd(strand_poller_t *p, int fd)
{
#ifdef STRAND_LINUX
	struct epoll_event ev;

	ev.events   = EPOLLIN;
	ev.data.ptr = NULL; /* sentinel: not a fiber fd table entry */
	if (epoll_ctl(p->pollfd, EPOLL_CTL_ADD, fd, &ev) == -1)
		return (STRAND_ERR_IO);
	return (STRAND_OK);
#endif

#ifdef STRAND_OPENBSD
	struct kevent kev;

	/*
	 * EV_ADD without EV_DISPATCH: persistent read filter.
	 * Fires every time the pipe has data, not just on the first edge.
	 * udata = NULL serves as the wakeup sentinel in poller_deliver_event.
	 */
	EV_SET(&kev, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (kevent(p->pollfd, &kev, 1, NULL, 0, NULL) == -1)
		return (STRAND_ERR_IO);
	return (STRAND_OK);
#endif
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */

/*
 * strand_fiber_wait_readable — park the current fiber until fd is readable.
 * See ARCHITECTURE.md §5 and include/strand.h for the full contract.
 */
int
strand_fiber_wait_readable(strand_scheduler_t *sched, int fd)
{
	return (fiber_wait_io(sched, fd, 0));
}

/*
 * strand_fiber_wait_writable — park the current fiber until fd is writable.
 * See ARCHITECTURE.md §5 and include/strand.h for the full contract.
 */
int
strand_fiber_wait_writable(strand_scheduler_t *sched, int fd)
{
	return (fiber_wait_io(sched, fd, 1));
}

