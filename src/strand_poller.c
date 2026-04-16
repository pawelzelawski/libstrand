/*
 * strand_poller.c — I/O multiplexing: epoll (Linux) and kqueue (OpenBSD).
 * See ARCHITECTURE.md §5.
 *
 * Task 4.1: strand_poller_t and fd waiter hash table implementation.
 */

#include "strand_poller.h"

#include <assert.h>
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
 *
 * Replaces the entry with a tombstone so that probe chains for other fds
 * that collided with this slot remain intact.
 *
 * Must only be called when both read_waiter and write_waiter are NULL —
 * i.e., after both I/O waiter fibers have been woken or cancelled.
 * Debug builds assert this precondition.
 *
 * No-op if fd is not found in the table.
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

