#include "u.h"
#include "lib.h"
#include "kern/dat.h"
#include "kern/fns.h"
#include "user.h"
#include "drawterm.h"

char *argv0;

void
sizebug(void)
{
	/*
	 * Needed by various parts of the code.
	 * This is a huge bug.
	 */
	assert(sizeof(char)==1);
	assert(sizeof(short)==2);
	assert(sizeof(ushort)==2);
	assert(sizeof(int)==4);
	assert(sizeof(uint)==4);
	assert(sizeof(long)==4);
	assert(sizeof(ulong)==4);
	assert(sizeof(vlong)==8);
	assert(sizeof(uvlong)==8);
}

int
main(int argc, char **argv)
{
	extern ulong kerndate;

	kerndate = seconds();
	eve = getuser();
	if(eve == nil)
		eve = "drawterm";

	sizebug();

	osinit();
	procinit0();
	printinit();

	chandevreset();
	chandevinit();
	quotefmtinstall();

	if(bind("#c", "/dev", MBEFORE) < 0)
		panic("bind #c: %r");
	if(bind("#e", "/env", MREPL|MCREATE) < 0)
		panic("bind #e: %r");
	if(bind("#I", "/net", MBEFORE) < 0)
		panic("bind #I: %r");
	if(bind("#U", "/root", MREPL) < 0)
		panic("bind #U: %r");
	bind("#A", "/dev", MAFTER);
	bind("#N", "/dev", MAFTER);
	bind("#C", "/", MAFTER);

	if(open("/dev/cons", OREAD) != 0)
		panic("open0: %r");
	if(open("/dev/cons", OWRITE) != 1)
		panic("open1: %r");
	if(open("/dev/cons", OWRITE) != 2)
		panic("open2: %r");

	cpumain(argc, argv);
	return 0;
}
