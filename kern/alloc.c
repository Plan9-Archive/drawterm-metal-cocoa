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

enum {
	SECMAGIC = 0x5ECA110C,
};

void*
secalloc(ulong n)
{
	void *p = mallocz(n+sizeof(ulong)*2, 1);
	((ulong*)p)[0] = SECMAGIC;
	((ulong*)p)[1] = n;
	return (ulong*)p+2;
}

void
secfree(void *p)
{
	if(p != nil){
		assert(((ulong*)p)[-2] == SECMAGIC);
		memset(p, 0, ((ulong*)p)[-1]);
		free((ulong*)p-2);
	}
}
