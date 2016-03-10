#include "u.h"
#include "libc.h"

uintptr
getcallerpc(void *a)
{
	return ((ulong*)a)[-1];
}
