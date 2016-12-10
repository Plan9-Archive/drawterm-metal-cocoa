#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

static Queue*	keyq;
static int kbdinuse;

void
kbdkey(Rune r, int down)
{
	char buf[2+UTFmax];

	if(r == 0)
		return;

	if(!kbdinuse || keyq == nil){
		if(down)
			kbdputc(kbdq, r);	/* /dev/cons */
		return;
	}

	memset(buf, 0, sizeof buf);
	buf[0] = down ? 'r' : 'R';
	qproduce(keyq, buf, 2+runetochar(buf+1, &r));
}

enum{
	Qdir,
	Qkbd,
};

static Dirtab kbddir[]={
	".",	{Qdir, 0, QTDIR},	0,		DMDIR|0555,
	"kbd",		{Qkbd},		0,		0444,
};

static void
kbdinit(void)
{
	keyq = qopen(4*1024, Qcoalesce, 0, 0);
	if(keyq == nil)
		panic("kbdinit");
	qnoblock(keyq, 1);
}

static Chan*
kbdattach(char *spec)
{
	return devattach('b', spec);
}

static Walkqid*
kbdwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name,nname, kbddir, nelem(kbddir), devgen);
}

static int
kbdstat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, kbddir, nelem(kbddir), devgen);
}

static Chan*
kbdopen(Chan *c, int omode)
{
	c = devopen(c, omode, kbddir, nelem(kbddir), devgen);
	switch((ulong)c->qid.path){
	case Qkbd:
		if(tas(&kbdinuse) != 0){
			c->flag &= ~COPEN;
			error(Einuse);
		}
		break;
	}
	return c;
}

static void
kbdclose(Chan *c)
{
	switch((ulong)c->qid.path){
	case Qkbd:
		if(c->flag&COPEN)
			kbdinuse = 0;
		break;
	}
}

static long
kbdread(Chan *c, void *buf, long n, vlong off)
{
	USED(off);

	if(n <= 0)
		return n;
	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, buf, n, kbddir, nelem(kbddir), devgen);
	case Qkbd:
		return qread(keyq, buf, n);
	default:
		print("kbdread 0x%llux\n", c->qid.path);
		error(Egreg);
	}
	return -1;		/* never reached */
}

static long
kbdwrite(Chan *c, void *va, long n, vlong off)
{
	USED(c);
	USED(va);
	USED(n);
	USED(off);
	error(Eperm);
	return -1;		/* never reached */
}

Dev kbddevtab = {
	'b',
	"kbd",

	devreset,
	kbdinit,
	devshutdown,
	kbdattach,
	kbdwalk,
	kbdstat,
	kbdopen,
	devcreate,
	kbdclose,
	kbdread,
	devbread,
	kbdwrite,
	devbwrite,
	devremove,
	devwstat,
};
