#include "u.h"
#include "libc.h"

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

int
tas(int *x)
{
#if __has_builtin(__atomic_test_and_set) || (defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 7)))
	return __atomic_test_and_set(x, __ATOMIC_ACQ_REL);
#else
	int     v,t, i = 1;

	__asm__ (
		"1:	ldxr	%0, [%2]\n"
		"	stxr	%w1, %3, [%2]\n"
		"	cmp	%1, #0\n"
		"	bne	1b"
		: "=&r" (v), "=&r" (t)
		: "r" (x), "r" (i)
		: "cc");

	switch(v) {
	case 0:
	case 1:
		return v;
	default:
		print("canlock: corrupted 0x%lux\n", v);
		return 1;
	}
#endif
}
