/*
 * strand_inject.c — minimal cross-worker cancel inject queue.
 *
 * Phase 4 needs enqueue-only cross-worker cancel routing for I/O waiters.
 * The full bounded MPSC ring buffer (capacity/backoff/order guarantees)
 * remains a Phase 5 deliverable.
 */

#include "strand_inject.h"

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

typedef struct strand_cancel_req {
	strand_fiber_handle_t handle;
	struct strand_cancel_req *next;
} strand_cancel_req_t;

int
inject_cancel_enqueue(strand_scheduler_t *sched, strand_fiber_handle_t handle)
{
	strand_cancel_req_t *req;

	if (sched == NULL)
		return (STRAND_ERR_WRONGCTX);

	req = malloc(sizeof(*req));
	if (req == NULL)
		return (STRAND_ERR_NOMEM);
	req->handle = handle;
	req->next = NULL;

	pthread_mutex_lock(&sched->inject_queue.inject_mu);
	if (sched->inject_queue.tail != NULL)
		sched->inject_queue.tail->next = req;
	else
		sched->inject_queue.head = req;
	sched->inject_queue.tail = req;
	pthread_mutex_unlock(&sched->inject_queue.inject_mu);

	/* Wake the owner worker so Step 1 can drain pending injected cancels. */
#ifdef STRAND_LINUX
	{
		uint64_t v = 1;
		(void)write(sched->wakeup_fd, &v, sizeof(v));
	}
#endif
#ifdef STRAND_OPENBSD
	{
		char v = 1;
		(void)write(sched->wakeup_pipe[1], &v, sizeof(v));
	}
#endif

	return (STRAND_OK);
}

void
inject_cancel_drain(strand_scheduler_t *sched)
{
	for (;;) {
		strand_cancel_req_t *req;

		pthread_mutex_lock(&sched->inject_queue.inject_mu);
		req = sched->inject_queue.head;
		if (req != NULL) {
			sched->inject_queue.head = req->next;
			if (sched->inject_queue.head == NULL)
				sched->inject_queue.tail = NULL;
		}
		pthread_mutex_unlock(&sched->inject_queue.inject_mu);

		if (req == NULL)
			break;

		(void)strand_fiber_cancel(req->handle);
		free(req);
	}
}

void
inject_cancel_discard_all(strand_scheduler_t *sched)
{
	for (;;) {
		strand_cancel_req_t *req;

		pthread_mutex_lock(&sched->inject_queue.inject_mu);
		req = sched->inject_queue.head;
		if (req != NULL) {
			sched->inject_queue.head = req->next;
			if (sched->inject_queue.head == NULL)
				sched->inject_queue.tail = NULL;
		}
		pthread_mutex_unlock(&sched->inject_queue.inject_mu);

		if (req == NULL)
			break;
		free(req);
	}
}

