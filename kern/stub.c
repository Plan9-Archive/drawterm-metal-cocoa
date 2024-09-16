#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

int
iseve(void)
{
	return 1;
}

void
splx(int x)
{
	USED(x);
}

int
splhi(void)
{
	return 0;
}

int
spllo(void)
{
	return 0;
}

long
hostdomainwrite(char *a, int n)
{
	USED(a);
	USED(n);
	error(Eperm);
	return 0;
}

long
hostownerwrite(char *a, int n)
{
	USED(a);
	USED(n);
	error(Eperm);
	return 0;
}

void
setmalloctag(void *v, uintptr tag)
{
	USED(v);
	USED(tag);
}

void
setrealloctag(void *v, uintptr tag)
{
	USED(v);
	USED(tag);
}

void
exhausted(char *s)
{
	panic("out of %s", s);
}
