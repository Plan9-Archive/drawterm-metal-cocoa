#include	<windows.h>
#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

Chan*
lfdchan(void *fd)
{
	Chan *c;
	
	c = newchan();
	c->type = devno('L', 0);
	c->aux = fd;
	c->path = newpath("fd");
	c->mode = ORDWR;
	c->qid.type = 0;
	c->qid.path = 0;
	c->qid.vers = 0;
	c->dev = 0;
	c->offset = 0;
	return c;
}

/*
 * only good for stdin/stdout/stderr
 */
int
lfdfd(int fd)
{
	HANDLE h;

	switch(fd){
	case 0: h = GetStdHandle(STD_INPUT_HANDLE); break;
	case 1: h = GetStdHandle(STD_OUTPUT_HANDLE); break;
	case 2: h = GetStdHandle(STD_ERROR_HANDLE); break;
	default:
		return -1;
	}
	if(h == INVALID_HANDLE_VALUE)
		return -1;
	return newfd(lfdchan((void*)h));
}

static Chan*
lfdattach(char *x)
{
	USED(x);
	
	error(Egreg);
	return nil;
}

static Walkqid*
lfdwalk(Chan *c, Chan *nc, char **name, int nname)
{
	USED(c);
	USED(nc);
	USED(name);
	USED(nname);
	
	error(Egreg);
	return nil;
}

static int
lfdstat(Chan *c, uchar *dp, int n)
{
	USED(c);
	USED(dp);
	USED(n);
	error(Egreg);
	return -1;
}

static Chan*
lfdopen(Chan *c, int omode)
{
	USED(c);
	USED(omode);
	error(Egreg);
	return nil;
}

static void
lfdclose(Chan *c)
{
	CloseHandle((HANDLE)c->aux);
}

static long
lfdread(Chan *c, void *buf, long n, vlong off)
{
	DWORD r;

	USED(off);	/* can't pread on pipes */
	if(!ReadFile((HANDLE)c->aux, buf, (DWORD)n, &r, NULL))
		oserror();
	return r;
}

static long
lfdwrite(Chan *c, void *buf, long n, vlong off)
{
	DWORD r;

	USED(off);	/* can't pread on pipes */
	if(!WriteFile((HANDLE)c->aux, buf, (DWORD)n, &r, NULL))
		oserror();
	return r;
}

Dev lfddevtab = {
	'L',
	"lfd",
	
	devreset,
	devinit,
	devshutdown,
	lfdattach,
	lfdwalk,
	lfdstat,
	lfdopen,
	devcreate,
	lfdclose,
	lfdread,
	devbread,
	lfdwrite,
	devbwrite,
	devremove,
	devwstat,
};
