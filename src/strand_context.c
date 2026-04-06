/*
 * strand_context.c — C wrapper around the assembly context swap.
 * errno save/restore, MXCSR/FPCR save/restore, sanitizer hooks.
 * See ARCHITECTURE.md §3.4, §3.7.
 */

#include "strand_context.h"
