#include <stdatomic.h>
#include <time.h>

#include <strand.h>

static void
mark_done(void *arg)
{
	atomic_store((_Atomic int *)arg, 1);
}

int
main(void)
{
	strand_runtime_t *runtime;
	strand_worker_t *worker;
	_Atomic int done = 0;
	struct timespec delay = { 0, 1000000L };

	runtime = strand_runtime_init(NULL);
	if (runtime == NULL)
		return (1);
	worker = strand_worker_start(runtime, NULL);
	if (worker == NULL || strand_runtime_spawn(runtime, mark_done, &done, 0,
	    worker, NULL) != STRAND_OK) {
		strand_runtime_destroy(runtime);
		return (1);
	}
	while (!atomic_load(&done))
		nanosleep(&delay, NULL);
	strand_runtime_destroy(runtime);
	return (0);
}
