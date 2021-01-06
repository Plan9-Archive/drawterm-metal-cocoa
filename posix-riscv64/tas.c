#include "u.h"
#include "libc.h"

int
tas(int *x)
{
	int v, i = 1;

	__asm__(
		"1:	lr.w t0, (%1)\n"
		"	sc.w t1, %2, (%1)\n"
		"	bnez t1, 1b\n"
		"       mv %0, t0"
		: "=r" (v)
		: "r" (x), "r" (i)
		: "t1", "t0"
	);

	switch(v) {
	case 0:
	case 1:
		return v;
	default:
		print("canlock: corrupted 0x%lux\n", v);
		return 1;
	}
}

