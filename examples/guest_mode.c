/*
 * guest_mode.c - minimal public guest-mode bootstrap example.
 *
 * Build with: make examples
 */

#include <fcntl.h>
#include <unistd.h>

#include <strand.h>

typedef struct {
	strand_scheduler_t *sched;
	int fd;
	int ran;
} guest_state_t;

static void
read_one(void *varg)
{
	guest_state_t *state = varg;
	char byte;

	if (strand_fiber_wait_readable(state->sched, state->fd) != STRAND_OK)
		return;
	if (read(state->fd, &byte, sizeof(byte)) == (ssize_t)sizeof(byte))
		state->ran = 1;
}

static int
set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	return (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1
	            ? -1
	            : 0);
}

static void
drain_until_idle(strand_scheduler_t *sched)
{
	while (strand_scheduler_advance(sched, NULL) == STRAND_SCHED_PROGRESS)
		;
}

int
main(void)
{
	guest_state_t state;
	strand_scheduler_t *sched;
	int fds[2];
	char byte = 'x';

	sched = strand_scheduler_create(NULL);
	if (sched == NULL || pipe(fds) == -1)
		return (1);
	if (set_nonblocking(fds[0]) != 0 || set_nonblocking(fds[1]) != 0)
		return (1);
	state.sched = sched;
	state.fd = fds[0];
	state.ran = 0;
	if (strand_scheduler_spawn(sched, read_one, &state, 0, NULL) !=
	    STRAND_OK)
		return (1);

	/* Before a host poll, drain every runnable fiber, not just one budget.
	 */
	drain_until_idle(sched);
	if (write(fds[1], &byte, sizeof(byte)) != (ssize_t)sizeof(byte))
		return (1);
	drain_until_idle(sched);
	close(fds[0]);
	close(fds[1]);
	strand_scheduler_destroy(sched);
	return (state.ran == 1 ? 0 : 1);
}
