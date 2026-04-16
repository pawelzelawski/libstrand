#ifndef STRAND_INJECT_H
#define STRAND_INJECT_H

/*
 * strand_inject.h — inject queue internal interface.
 * Bounded MPSC ring buffer, one per worker. See ARCHITECTURE.md §6.3.
 */

#include "../include/strand.h"
#include "strand_internal.h"

/*
 * Minimal Phase 4 inject support: enqueue/dequeue cross-worker cancel
 * requests. Full bounded MPSC queue lands in Phase 5.
 */
int inject_cancel_enqueue(strand_scheduler_t *sched,
						  strand_fiber_handle_t handle);
void inject_cancel_drain(strand_scheduler_t *sched);
void inject_cancel_discard_all(strand_scheduler_t *sched);

#endif /* STRAND_INJECT_H */
