#ifndef STRAND_RUNTIME_H
#define STRAND_RUNTIME_H

/*
 * strand_runtime.h - multi-worker runtime internal interface.
 * See ARCHITECTURE.md §6.1, §6.2, §6.4.
 */

#include "../include/strand.h"
#include "strand_internal.h"

/*
 * runtime_lock / runtime_unlock - acquire/release the spinlock that
 * protects workers[] and worker_count.
 *
 * Implemented as a CAS-based test-and-set spinlock using _Atomic int.
 * The spinlock is held only at strand_worker_start and strand_runtime_spawn
 * time - low-frequency host-thread operations.
 */
void runtime_lock(strand_runtime_t *rt);
void runtime_unlock(strand_runtime_t *rt);

/*
 * runtime_select_worker - round-robin worker selection.
 *
 * Atomically increments rr_counter and selects workers[counter % count].
 * Skips workers whose stop_flag is set.  Returns NULL if all workers are
 * stopped or worker_count is zero.
 *
 * Must be called with the spinlock held.
 * See ARCHITECTURE.md §6.4.
 */
strand_worker_t *runtime_select_worker(strand_runtime_t *rt);

#endif /* STRAND_RUNTIME_H */
