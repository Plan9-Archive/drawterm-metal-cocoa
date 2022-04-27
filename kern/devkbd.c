#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"
#include	"keyboard.h"

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

int code2key[] = {
	0, Kesc, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\x08',
	'\x09', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	Kctl, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', Kshift,
	'\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', Kshift, '*', Kalt,
	' ', Kcaps, KF|1, KF|2, KF|3, KF|4, KF|5, KF|6, KF|7, KF|8, KF|9, KF|10,
	Knum, Kscroll,
	'7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};
int code2key_shift[] = {
	0, Kesc, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\x08',
	'\x09', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	Kctl, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', Kshift,
	'|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', Kshift, '*', Kalt,
	' ', Kcaps, KF|1, KF|2, KF|3, KF|4, KF|5, KF|6, KF|7, KF|8, KF|9, KF|10,
	Knum, Kscroll,
	'7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

void
kbdsc(int k)
{
	static int shift;
	int down;
	uchar buf[2];

	if(k == 0)
		return;
	if(!kbdinuse || keyq == nil){
		/* manual transformation for /dev/cons */
		down = !(k & 0x80);
		k &= 0xff7f;
		if(down || shift){
			switch(k){
			case 0xe047:	k = Khome;	break;
			case 0xe048:	k = Kup;	break;
			case 0xe049:	k = Kpgup;	break;
			case 0xe04b:	k = Kleft;	break;
			case 0xe04d:	k = Kright;	break;
			case 0xe04f:	k = Kend;	break;
			case 0xe050:	k = Kdown;	break;
			case 0xe051:	k = Kpgdown;	break;
			case 0xe052:	k = Kins;	break;
			case 0xe053:	k = Kdel;	break;
			default:
				if(k < nelem(code2key)){
					if(shift)
						k = code2key_shift[k];
					else
						k = code2key[k];
				}
			}
			if(k == Kshift)
				shift = down;
			else if(down)
				kbdputc(kbdq, k);
		}
		return;
	}
	k &= 0xffff;
	if(k > 0xff){
		buf[0] = k >> 8;
		buf[1] = k & 0xff;
		qproduce(keyq, buf, 2);
	}else{
		buf[0] = k;
		qproduce(keyq, buf, 1);
	}
}

enum{
	Qdir,
	Qkbd,
	Qscancode,
};

static Dirtab kbddir[]={
	".",	{Qdir, 0, QTDIR},	0,		DMDIR|0555,
	"kbd",		{Qkbd},		0,		0444,
	"scancode",	{Qscancode},		0,		0444,
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
#ifdef KBDSCANCODE
	if(c->qid.path == Qkbd)
#else
	if(c->qid.path == Qscancode)
#endif
		error(Eperm);
	else
	switch((ulong)c->qid.path){
	case Qkbd:
	case Qscancode:
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
	case Qscancode:
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
	case Qscancode:
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
