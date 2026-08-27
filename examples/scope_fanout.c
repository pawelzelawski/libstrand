#include <strand.h>

typedef struct {
	strand_scheduler_t *sched;
	int completed;
} scope_state_t;

static int
child(void *arg)
{
	(*(int *)arg)++;
	return (0);
}

static void
root(void *arg)
{
	scope_state_t *state = arg;
	strand_scope_t *scope = strand_scope_create();

	if (scope == NULL)
		return;
	if (strand_scope_open(state->sched, scope) == STRAND_OK &&
	    strand_scope_spawn(state->sched, scope, child, &state->completed,
	    NULL) == STRAND_OK && strand_scope_spawn(state->sched, scope, child,
	    &state->completed, NULL) == STRAND_OK)
		(void)strand_scope_wait(state->sched, scope);
	strand_scope_destroy(scope);
}

int
main(void)
{
	scope_state_t state = { 0 };

	state.sched = strand_scheduler_create(NULL);
	if (state.sched == NULL || strand_scheduler_spawn(state.sched, root,
	    &state, 0, NULL) != STRAND_OK)
		return (1);
	while (strand_scheduler_advance(state.sched, NULL) == STRAND_SCHED_PROGRESS)
		;
	strand_scheduler_destroy(state.sched);
	return (state.completed == 2 ? 0 : 1);
}
