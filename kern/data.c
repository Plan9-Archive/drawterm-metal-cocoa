#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

Conf conf = 
{
	1,	/* processors */
	100,	/* processes */
	0,	/* size in bytes of pipe queues */
};

char *eve = "eve";
ulong kerndate;
int cpuserver;
char hostdomain[] = "drawterm.net";
