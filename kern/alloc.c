#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

void*
smalloc(ulong n)
{
	return mallocz(n, 1);
}

void*
malloc(ulong n)
{
	return mallocz(n, 1);
}

void*
secalloc(ulong n)
{
	void *p = mallocz(n+sizeof(ulong), 1);
	*(ulong*)p = n;
	return (ulong*)p+1;
}

void
secfree(void *p)
{
	if(p != nil){
		memset(p, 0, ((ulong*)p)[-1]);
		free((ulong*)p-1);
	}
}
