#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

#include 	"keyboard.h"

#include	<authsrv.h>

#undef write
#undef read

void	(*screenputs)(char*, int) = 0;

Kmesg	kmesg;			/* console messages */
Queue*	kbdq;			/* unprocessed console input */
Queue*	lineq;			/* processed console input */
Queue*	kprintoq;		/* console output, for /dev/kprint */
int	kprintinuse;		/* test and set whether /dev/kprint is open */

int	panicking;

struct
{
	int exiting;
	int machs;
} active;

static struct
{
	QLock lk;

	int	raw;		/* true if we shouldn't process input */
	int	ctl;		/* number of opens to the control file */
	int	x;		/* index into line */
	char	line[1024];	/* current input line */

	int	count;

	/* a place to save up characters at interrupt time before dumping them in the queue */
	Lock	lockputc;
	char	istage[1024];
	char	*iw;
	char	*ir;
	char	*ie;
} kbd = {
	{ 0 },
	0,
	0,
	0,
	{ 0 },
	0,
	{ 0 },
	{ 0 },
	kbd.istage,
	kbd.istage,
	kbd.istage + sizeof(kbd.istage),
};

char	*sysname;
vlong	fasthz = 1000;

static int	readtime(ulong, char*, int);
static int	readbintime(char*, int);
static int	writetime(char*, int);
static int	writebintime(char*, int);

enum
{
	CMreboot,
	CMpanic,
};

Cmdtab rebootmsg[] =
{
	CMreboot,	"reboot",	0,
	CMpanic,	"panic",	0,
};

int
return0(void *v)
{
	return 0;
}

void
printinit(void)
{
	lineq = qopen(2*1024, 0, 0, nil);
	if(lineq == nil)
		panic("printinit");
	qnoblock(lineq, 1);

	kbdq = qopen(4*1024, 0, 0, 0);
	if(kbdq == nil)
		panic("kbdinit");
	qnoblock(kbdq, 1);
}

static void
kmesgputs(char *str, int n)
{
	uint nn, d;

	ilock(&kmesg.lk);
	/* take the tail of huge writes */
	if(n > sizeof kmesg.buf){
		d = n - sizeof kmesg.buf;
		str += d;
		n -= d;
	}

	/* slide the buffer down to make room */
	nn = kmesg.n;
	if(nn + n >= sizeof kmesg.buf){
		d = nn + n - sizeof kmesg.buf;
		if(d)
			memmove(kmesg.buf, kmesg.buf+d, sizeof kmesg.buf-d);
		nn -= d;
	}

	/* copy the data in */
	memmove(kmesg.buf+nn, str, n);
	nn += n;
	kmesg.n = nn;
	iunlock(&kmesg.lk);
}

/*
 *   Print a string on the console.  Convert \n to \r\n for serial
 *   line consoles.  Locking of the queues is left up to the screen
 *   or uart code.  Multi-line messages to serial consoles may get
 *   interspersed with other messages.
 */
static void
putstrn0(char *str, int n, int usewrite)
{
	/*
	 *  how many different output devices do we need?
	 */
	kmesgputs(str, n);

	/*
	 *  if someone is reading /dev/kprint,
	 *  put the message there.
	 *  if not and there's an attached bit mapped display,
	 *  put the message there.
	 *
	 *  if there's a serial line being used as a console,
	 *  put the message there.
	 */
	if(kprintoq != nil && !qisclosed(kprintoq)){
		if(usewrite)
			qwrite(kprintoq, str, n);
		else
			qiwrite(kprintoq, str, n);
	}else if(screenputs != 0)
		screenputs(str, n);
	else
		write(1, str, n);
}

void
putstrn(char *str, int n)
{
	putstrn0(str, n, 0);
}

int
print(char *fmt, ...)
{
	int n;
	va_list arg;
	char buf[PRINTSIZE];

	va_start(arg, fmt);
	n = vseprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);
	putstrn(buf, n);

	return n;
}

void
panic(char *fmt, ...)
{
	int n;
	va_list arg;
	char buf[PRINTSIZE];

	kprintoq = nil;	/* don't try to write to /dev/kprint */

	if(panicking++)
		for(;;) osyield();
	splhi();
	strcpy(buf, "panic: ");
	va_start(arg, fmt);
	n = vseprint(buf+strlen(buf), buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);
	buf[n] = '\n';
	spllo();
	putstrn(buf, n+1);
	while(screenputs != 0)
		osyield();
	setterm(0);
	exit(1);
}

int
pprint(char *fmt, ...)
{
	int n;
	Chan *c;
	va_list arg;
	char buf[2*PRINTSIZE];

	if(up == nil || up->fgrp == nil)
		return 0;

	c = up->fgrp->fd[2];
	if(c==0 || (c->mode!=OWRITE && c->mode!=ORDWR))
		return 0;
	n = sprint(buf, "%s %lud: ", up->text, up->pid);
	va_start(arg, fmt);
	n = vseprint(buf+n, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);

	if(waserror())
		return 0;
	devtab[c->type]->write(c, buf, n, c->offset);
	poperror();

	lock(&c->ref.lk);
	c->offset += n;
	unlock(&c->ref.lk);

	return n;
}

static void
echoscreen(char *buf, int n)
{
	char ebuf[128];
	char *e, *p;
	int x;

	p = ebuf;
	e = ebuf + sizeof(ebuf) - 4;
	while(n-- > 0){
		if(p >= e){
			screenputs(ebuf, p - ebuf);
			p = ebuf;
		}
		x = *buf++;
		if(x == 0x15){
			*p++ = '^';
			*p++ = 'U';
			*p++ = '\n';
		} else
			*p++ = x;
	}
	if(p != ebuf)
		screenputs(ebuf, p - ebuf);
}

static void
echo(char *buf, int n)
{
	if(kbd.raw)
		return;
	if(screenputs != 0)
		echoscreen(buf, n);
	else
		write(1, buf, n);
}

static int
_kbdputc(Queue *q, int c)
{
	char buf[UTFmax];
	Rune r = c;
	int n;

	if((n = runetochar(buf, &r)) > 0){
		echo(buf, n);
		qproduce(q, buf, n);
	}
	return 0;
}

/* _kbdputc, but with compose translation */
int
kbdputc(Queue *q, int c)
{
	static int collecting, nk;
	static Rune kc[5];
	int i;

	switch(c){
	case 0:
	case Kcaps:
	case Knum:
	case Kshift:
	case Kaltgr:
	case Kmod4:
	case Kctl:
		/* ignore modifiers; see nextrune() of kbdfs */
		return 0;

	case Kalt:
		collecting = 1;
		nk = 0;
		return 0;
	}

	if(!collecting)
		return _kbdputc(q, c);

	kc[nk++] = c;
	c = latin1(kc, nk);
	if(c < -1)  /* need more keystrokes */
		return 0;
	if(c != -1) /* valid sequence */
		_kbdputc(q, c);
	else
		for(i=0; i<nk; i++)
			_kbdputc(q, kc[i]);
	nk = 0;
	collecting = 0;
	return 0;
}


enum{
	Qdir,
	Qbintime,
	Qcons,
	Qconsctl,
	Qdrivers,
	Qkmesg,
	Qkprint,
	Qhostdomain,
	Qhostowner,
	Qnull,
	Qosversion,
	Qrandom,
	Qreboot,
	Qshowfile,
	Qsnarf,
	Qsysname,
	Qsysstat,
	Qtime,
	Qzero,
};

enum
{
	VLNUMSIZE=	22,
};

static Dirtab consdir[]={
	".",	{Qdir, 0, QTDIR},	0,		DMDIR|0555,
	"bintime",	{Qbintime},	24,		0664,
	"cons",		{Qcons},	0,		0660,
	"consctl",	{Qconsctl},	0,		0220,
	"drivers",	{Qdrivers},	0,		0444,
	"hostdomain",	{Qhostdomain},	DOMLEN,		0664,
	"hostowner",	{Qhostowner},	0,	0664,
	"kmesg",	{Qkmesg},	0,		0440,
	"kprint",	{Qkprint, 0, QTEXCL},	0,	DMEXCL|0440,
	"null",		{Qnull},	0,		0666,
	"osversion",	{Qosversion},	0,		0444,
	"random",	{Qrandom},	0,		0444,
	"reboot",	{Qreboot},	0,		0664,
	"showfile",	{Qshowfile},	0,	0220,
	"snarf",	{Qsnarf},		0,		0666,
	"sysname",	{Qsysname},	0,		0664,
	"sysstat",	{Qsysstat},	0,		0666,
	"time",		{Qtime},	NUMSIZE+3*VLNUMSIZE,	0664,
	"zero",		{Qzero},	0,		0444,
};

Dirtab *snarftab = &consdir[Qsnarf];

int
readnum(ulong off, char *buf, ulong n, ulong val, int size)
{
	char tmp[64];

	snprint(tmp, sizeof(tmp), "%*.0lud", size-1, val);
	tmp[size-1] = ' ';
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, tmp+off, n);
	return n;
}

int
readstr(ulong off, char *buf, ulong n, char *str)
{
	int size;

	size = strlen(str);
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, str+off, n);
	return n;
}

static void
consinit(void)
{
	randominit();
}

static Chan*
consattach(char *spec)
{
	return devattach('c', spec);
}

static Walkqid*
conswalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name,nname, consdir, nelem(consdir), devgen);
}

static int
consstat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, consdir, nelem(consdir), devgen);
}

static Chan*
consopen(Chan *c, int omode)
{
	c->aux = nil;
	c = devopen(c, omode, consdir, nelem(consdir), devgen);
	switch((ulong)c->qid.path){
	case Qconsctl:
		qlock(&kbd.lk);
		kbd.ctl++;
		qunlock(&kbd.lk);
		break;

	case Qkprint:
		if(tas(&kprintinuse) != 0){
			c->flag &= ~COPEN;
			error(Einuse);
		}
		if(kprintoq == nil){
			kprintoq = qopen(8*1024, Qcoalesce, 0, 0);
			if(kprintoq == nil){
				c->flag &= ~COPEN;
				error(Enomem);
			}
			qnoblock(kprintoq, 1);
		}else
			qreopen(kprintoq);
		c->iounit = qiomaxatomic;
		break;

	case Qsnarf:
		if(omode == ORDWR)
			error(Eperm);
		if(omode == OREAD)
			c->aux = strdup("");
		else
			c->aux = mallocz(SnarfSize, 1);
		break;
	}
	return c;
}

static void
consclose(Chan *c)
{
	switch((ulong)c->qid.path){
	/* last close of control file turns off raw */
	case Qconsctl:
		if(c->flag&COPEN){
			qlock(&kbd.lk);
			if(--kbd.ctl == 0)
				kbd.raw = 0;
			if(screenputs == 0)
				setterm(kbd.raw);
			qunlock(&kbd.lk);
		}
		break;

	/* close of kprint allows other opens */
	case Qkprint:
		if(c->flag & COPEN){
			kprintinuse = 0;
			qhangup(kprintoq, nil);
		}
		break;

	case Qsnarf:
		if(c->mode == OWRITE)
			clipwrite(c->aux);
		free(c->aux);
		break;
	}
}

static int
qreadcons(Queue *q, char *buf, int n)
{
	if(screenputs==0 && !qcanread(q))
		return read(0, buf, n);
	return qread(q, buf, n);
}

static long
consread(Chan *c, void *buf, long n, vlong off)
{
	char *b;
	char tmp[128];		/* must be >= 6*NUMSIZE */
	char *cbuf = buf;
	int ch, i, eol;
	vlong offset = off;

	if(n <= 0)
		return n;
	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, buf, n, consdir, nelem(consdir), devgen);

	case Qcons:
		qlock(&kbd.lk);
		if(waserror()) {
			qunlock(&kbd.lk);
			nexterror();
		}
		if(kbd.raw) {
			if(qcanread(lineq))
				n = qread(lineq, buf, n);
			else {
				/* read as much as possible */
				do {
					i = qreadcons(kbdq, cbuf, n);
					cbuf += i;
					n -= i;
				} while (n>0 && qcanread(kbdq));
				n = cbuf - (char*)buf;
			}
		} else {
			while(!qcanread(lineq)) {
				eol = 1;
				if(qreadcons(kbdq, &kbd.line[kbd.x], 1) == 1){
					eol = 0;
					ch = kbd.line[kbd.x];
					switch(ch){
					case '\b':
						if(kbd.x)
							kbd.x--;
						break;
					case 0x15:
						kbd.x = 0;
						break;
					case '\n':
						kbd.x++;
					case 0x04:
						eol = 1;
						break;
					default:
						kbd.x++;
					}
				}
				if(kbd.x == sizeof(kbd.line) || eol){
					qwrite(lineq, kbd.line, kbd.x);
					kbd.x = 0;
				}
			}
			n = qread(lineq, buf, n);
		}
		qunlock(&kbd.lk);
		poperror();
		return n;

	case Qkmesg:
		/*
		 * This is unlocked to avoid tying up a process
		 * that's writing to the buffer.  kmesg.n never 
		 * gets smaller, so worst case the reader will
		 * see a slurred buffer.
		 */
		if(off >= kmesg.n)
			n = 0;
		else{
			if(off+n > kmesg.n)
				n = kmesg.n - off;
			memmove(buf, kmesg.buf+off, n);
		}
		return n;
		
	case Qkprint:
		return qread(kprintoq, buf, n);

	case Qtime:
		return readtime((ulong)offset, buf, n);

	case Qbintime:
		return readbintime(buf, n);

	case Qhostowner:
		return readstr((ulong)offset, buf, n, eve);

	case Qhostdomain:
		return readstr((ulong)offset, buf, n, hostdomain);

	case Qnull:
		return 0;

	case Qsnarf:
		if(offset == 0){
			free(c->aux);
			c->aux = clipread();
		}
		if(c->aux == nil)
			return 0;
		return readstr(offset, buf, n, c->aux);

	case Qsysstat:
		return 0;

	case Qsysname:
		if(sysname == nil)
			return 0;
		return readstr((ulong)offset, buf, n, sysname);

	case Qrandom:
		return randomread(buf, n);

	case Qdrivers:
		b = malloc(READSTR);
		if(b == nil)
			error(Enomem);
		n = 0;
		for(i = 0; devtab[i] != nil; i++)
			n += snprint(b+n, READSTR-n, "#%C %s\n", devtab[i]->dc,  devtab[i]->name);
		if(waserror()){
			free(b);
			nexterror();
		}
		n = readstr((ulong)offset, buf, n, b);
		free(b);
		poperror();
		return n;

	case Qzero:
		memset(buf, 0, n);
		return n;

	case Qosversion:
		snprint(tmp, sizeof tmp, "2000");
		n = readstr((ulong)offset, buf, n, tmp);
		return n;

	default:
		print("consread 0x%llux\n", c->qid.path);
		error(Egreg);
	}
	return -1;		/* never reached */
}

static long
conswrite(Chan *c, void *va, long n, vlong off)
{
	char buf[256];
	long l, bp;
	char *a = va;
	ulong offset = off;
	Cmdbuf *cb;
	Cmdtab *ct;

	switch((ulong)c->qid.path){
	case Qcons:
		/*
		 * Can't page fault in putstrn, so copy the data locally.
		 */
		l = n;
		while(l > 0){
			bp = l;
			if(bp > sizeof buf)
				bp = sizeof buf;
			memmove(buf, a, bp);
			putstrn0(buf, bp, 1);
			a += bp;
			l -= bp;
		}
		break;

	case Qconsctl:
		if(n >= sizeof(buf))
			n = sizeof(buf)-1;
		strncpy(buf, a, n);
		buf[n] = 0;
		for(a = buf; a;){
			if(strncmp(a, "rawon", 5) == 0){
				qlock(&kbd.lk);
				if(kbd.x){
					qwrite(kbdq, kbd.line, kbd.x);
					kbd.x = 0;
				}
				kbd.raw = 1;
				if(screenputs == 0)
					setterm(1);
				qunlock(&kbd.lk);
			} else if(strncmp(a, "rawoff", 6) == 0){
				qlock(&kbd.lk);
				kbd.raw = 0;
				kbd.x = 0;
				if(screenputs == 0)
					setterm(0);
				qunlock(&kbd.lk);
			}
			if((a = strchr(a, ' ')))
				a++;
		}
		break;

	case Qtime:
		if(!iseve())
			error(Eperm);
		return writetime(a, n);

	case Qbintime:
		if(!iseve())
			error(Eperm);
		return writebintime(a, n);

	case Qhostowner:
		return hostownerwrite(a, n);

	case Qhostdomain:
		return hostdomainwrite(a, n);

	case Qnull:
		break;

	case Qreboot:
		if(!iseve())
			error(Eperm);
		cb = parsecmd(a, n);

		if(waserror()) {
			free(cb);
			nexterror();
		}
		ct = lookupcmd(cb, rebootmsg, nelem(rebootmsg));
		switch(ct->index) {
		case CMreboot:
			error(Egreg);
			break;
		case CMpanic:
			panic("/dev/reboot");
		}
		poperror();
		free(cb);
		break;

	case Qshowfile:
		return showfilewrite(a, n);

	case Qsnarf:
		if(offset >= SnarfSize || offset+n >= SnarfSize)
			error(Etoobig);
		snarftab->qid.vers++;
		memmove((uchar*)c->aux+offset, va, n);
		return n;

	case Qsysstat:
		n = 0;
		break;

	case Qsysname:
		if(offset != 0)
			error(Ebadarg);
		if(n <= 0 || n >= sizeof buf)
			error(Ebadarg);
		strncpy(buf, a, n);
		buf[n] = 0;
		if(buf[n-1] == '\n')
			buf[n-1] = 0;
		kstrdup(&sysname, buf);
		break;

	default:
		print("conswrite: 0x%llux\n", c->qid.path);
		error(Egreg);
	}
	return n;
}

Dev consdevtab = {
	'c',
	"cons",

	devreset,
	consinit,
	devshutdown,
	consattach,
	conswalk,
	consstat,
	consopen,
	devcreate,
	consclose,
	consread,
	devbread,
	conswrite,
	devbwrite,
	devremove,
	devwstat,
};

static uvlong uvorder = (uvlong) 0x0001020304050607ULL;

static uchar*
vlong2le(uchar *t, vlong from)
{
	uchar *f, *o;
	int i;

	f = (uchar*)&from;
	o = (uchar*)&uvorder;
	for(i = 0; i < sizeof(vlong); i++)
		t[i] = f[o[i]];
	return t+sizeof(vlong);
}

char *Ebadtimectl = "bad time control";

/*
 *  like the old #c/time but with added info.  Return
 *
 *	secs	nanosecs	fastticks	fasthz
 */
static int
readtime(ulong off, char *buf, int n)
{
	vlong nsec;
	ulong sec;
	char str[7*NUMSIZE];

	sec = seconds();
	nsec = (vlong)sec*1000000000LL;
	snprint(str, sizeof(str), "%*.0lud %*.0llud %*.0llud %*.0llud ",
		NUMSIZE-1, sec,
		VLNUMSIZE-1, nsec,
		VLNUMSIZE-1, ticks(),
		VLNUMSIZE-1, fasthz);
	return readstr(off, buf, n, str);
}

/*
 *  set the time in seconds
 */
static int
writetime(char *buf, int n)
{
	USED(buf);
	USED(n);
	error(Egreg);
	return 0;
}

/*
 *  read binary time info.  all numbers are little endian.
 *  ticks and nsec are syncronized.
 */
static int
readbintime(char *buf, int n)
{
	int i;
	vlong nsec;
	uchar *b = (uchar*)buf;

	i = 0;
	nsec = (ulong)seconds()*1000000000LL;
	if(n >= 3*sizeof(uvlong)){
		vlong2le(b+2*sizeof(uvlong), fasthz);
		i += sizeof(uvlong);
	}
	if(n >= 2*sizeof(uvlong)){
		vlong2le(b+sizeof(uvlong), ticks());
		i += sizeof(uvlong);
	}
	if(n >= 8){
		vlong2le(b, nsec);
		i += sizeof(vlong);
	}
	return i;
}

/*
 *  set any of the following
 *	- time in nsec
 *	- nsec trim applied over some seconds
 *	- clock frequency
 */
static int
writebintime(char *buf, int n)
{
	USED(buf);
	USED(n);
	error(Egreg);
	return 0;
}


int
iprint(char *fmt, ...)
{
	int n, s;
	va_list arg;
	char buf[PRINTSIZE];

	s = splhi();
	va_start(arg, fmt);
	n = vseprint(buf, buf+sizeof(buf), fmt, arg) - buf;
	va_end(arg);
	write(2, buf, n);
	if(screenputs != 0)
		screenputs(buf, n);
	splx(s);

	return n;
}
