/*
 * strand_fiber.c — fiber descriptor lifecycle, stack cache, fiber-local
 * storage. See ARCHITECTURE.md §4.5, §13.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "strand_fiber.h"
#include "strand_internal.h"

/*
 * page_size() — system page size, cached after the first call.
 * sysconf(_SC_PAGESIZE) is idempotent; the one-time race in a multi-threaded
 * context is safe because both threads would write the same value.
 */
size_t
page_size(void)
{
	static size_t cached;
	long v;

	if (cached != 0)
		return cached;

	v = sysconf(_SC_PAGESIZE);
	if (v <= 0)
		return 0;

	cached = (size_t)v;
	return cached;
}

/*
 * stack_alloc — allocate a fiber stack with a guard page.
 *
 * Layout of the mmap region (low address to high address):
 *
 *   [guard page — PROT_NONE][usable stack — PROT_READ|PROT_WRITE]
 *   <-- PAGE_SIZE ----------><-- stack_size ---------------------->
 *
 * The stack grows downward; the fiber uses the top of the usable region.
 * Returns the mmap base on success, NULL on failure.
 * *vg_id_out receives the Valgrind stack registration ID.
 *
 * The caller is responsible for storing both the returned base pointer
 * and the Valgrind ID for later use with stack_free.  The usable base
 * (suitable for strand_context_init stack_top) is base + PAGE_SIZE +
 * stack_size (one past the top of the usable region).
 */
void *
stack_alloc(size_t stack_size, unsigned long *vg_id_out)
{
	size_t pgsz = page_size();
	size_t total;
	void *base;

	if (vg_id_out == NULL || pgsz == 0 || stack_size == 0)
		return NULL;
	if (SIZE_MAX - pgsz < stack_size)
		return NULL;
	total = stack_size + pgsz;

	base = mmap(NULL, total, PROT_READ | PROT_WRITE,
	            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (base == MAP_FAILED)
		return NULL;

	if (mprotect(base, pgsz, PROT_NONE) != 0) {
		munmap(base, total);
		return NULL;
	}

	/*
	 * Register the usable portion — from just above the guard page to
	 * the top of the mapped region — with Valgrind.  This is a no-op when
	 * not running under Valgrind.  See TECH_STACK.md §7.1.
	 */
	*vg_id_out = STRAND_VG_STACK_REGISTER((char *)base + pgsz,
	                                      (char *)base + pgsz + stack_size);

	return base;
}

/*
 * stack_free — deregister the fiber stack from Valgrind and unmap it.
 *
 * base must be the mmap base returned by stack_alloc (not the usable base).
 * vg_id must be the Valgrind ID returned via stack_alloc's vg_id_out.
 */
void
stack_free(void *base, size_t stack_size, unsigned long vg_id)
{
	size_t pgsz = page_size();

	if (base == NULL || pgsz == 0 || stack_size == 0)
		return;
	if (SIZE_MAX - pgsz < stack_size)
		return;

	STRAND_VG_STACK_DEREGISTER(vg_id);
	munmap(base, stack_size + pgsz);
}
/*
 * strand_fiber_tsan_init — initialise the TSan fiber handle on a newly
 * allocated strand_fiber_t.  Must be called once per descriptor before any
 * context switch involving this fiber.  In non-TSan builds the macro is a
 * no-op.
 * See ARCHITECTURE.md §3.7 and TECH_STACK.md §7.3.
 */
void
strand_fiber_tsan_init(strand_fiber_t *f)
{
	if (f == NULL)
		return;

	f->tsan_fiber = NULL;
	STRAND_TSAN_CREATE(f);
}

/*
 * strand_fiber_tsan_destroy — destroy the TSan handle when a strand_fiber_t is
 * being freed or returned to the dead pool.  Must be called after the fiber
 * has finished and will never be switched to again.  In non-TSan builds the
 * macro is a no-op.
 * See ARCHITECTURE.md §3.7 and TECH_STACK.md §7.3.
 */
void
strand_fiber_tsan_destroy(strand_fiber_t *f)
{
	if (f == NULL)
		return;

	if (f->tsan_fiber != NULL)
		STRAND_TSAN_DESTROY(f);
	f->tsan_fiber = NULL;
}

/*
 * strand_fiber_tsan_bind_current — bind descriptor to current thread/fiber
 * TSan context. This is for scheduler/root contexts that represent the
 * currently running thread rather than a separately created fiber context.
 */
void
strand_fiber_tsan_bind_current(strand_fiber_t *f)
{
	if (f == NULL)
		return;

	f->tsan_fiber = NULL;
	STRAND_TSAN_BIND_CURRENT(f);
}
