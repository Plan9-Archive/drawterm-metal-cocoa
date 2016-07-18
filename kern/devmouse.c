#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

#include	"draw.h"
#include	"memdraw.h"
#include	"screen.h"

int	mousequeue = 1;

Mouseinfo	mouse;
Cursorinfo	cursor;
Cursorinfo      arrow = {
	0,
	{ -1, -1 },
	{ 0xFF, 0xFF, 0x80, 0x01, 0x80, 0x02, 0x80, 0x0C,
	  0x80, 0x10, 0x80, 0x10, 0x80, 0x08, 0x80, 0x04,
	  0x80, 0x02, 0x80, 0x01, 0x80, 0x02, 0x8C, 0x04,
	  0x92, 0x08, 0x91, 0x10, 0xA0, 0xA0, 0xC0, 0x40,
	},
	{ 0x00, 0x00, 0x7F, 0xFE, 0x7F, 0xFC, 0x7F, 0xF0,
	  0x7F, 0xE0, 0x7F, 0xE0, 0x7F, 0xF0, 0x7F, 0xF8,
	  0x7F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFC, 0x73, 0xF8,
	  0x61, 0xF0, 0x60, 0xE0, 0x40, 0x40, 0x00, 0x00,
	},
};

static uchar	buttonmap[8] = {0,1,2,3,4,5,6,7,};
static int	mouseswap;
static int	scrollswap;

static int	mousechanged(void*);

enum {
	CMbuttonmap,
	CMscrollswap,
	CMswap,
};

static Cmdtab mousectlmsg[] = 
{
	CMbuttonmap,	"buttonmap",	0,
	CMscrollswap,	"scrollswap",	0,
	CMswap,		"swap",		1,
};

enum {
	Qdir,
	Qcursor,
	Qmouse,
	Qmousectl
};

Dirtab mousedir[]={
	".",		{Qdir, 0, QTDIR},	0,	DMDIR|0555,	
	"cursor",	{Qcursor},		0,	0666,
	"mouse",	{Qmouse},		0,	0666,
	"mousectl",	{Qmousectl},		0,	0222,
};

#define	NMOUSE	(sizeof(mousedir)/sizeof(Dirtab))

static void
mouseinit(void)
{
	cursor = arrow;
}

static Chan*
mouseattach(char *spec)
{
	setcursor();
	return devattach('m', spec);
}

static Walkqid*
mousewalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, mousedir, NMOUSE, devgen);
}

static int
mousestat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, mousedir, NMOUSE, devgen);
}

static Chan*
mouseopen(Chan *c, int omode)
{
	switch((long)c->qid.path){
	case Qdir:
		if(omode != OREAD)
			error(Eperm);
		break;
	case Qmouse:
		lock(&mouse.lk);
		if(mouse.open){
			unlock(&mouse.lk);
			error(Einuse);
		}
		mouse.open = 1;
		unlock(&mouse.lk);
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

void
mouseclose(Chan *c)
{
	if(!(c->flag&COPEN))
		return;

	switch((long)c->qid.path) {
	case Qmouse:
		lock(&mouse.lk);
		mouse.open = 0;
		unlock(&mouse.lk);
		cursor = arrow;
		setcursor();
	}
}

long
mouseread(Chan *c, void *va, long n, vlong offset)
{
	char buf[4*12+1];
	uchar *p;
	int i, nn, b;
	ulong msec;
	
	p = va;
	switch((long)c->qid.path){
	case Qdir:
		return devdirread(c, va, n, mousedir, NMOUSE, devgen);

	case Qcursor:
		if(offset != 0)
			return 0;
		if(n < 2*4+2*2*16)
			error(Eshort);
		n = 2*4+2*2*16;
		lock(&cursor.lk);
		BPLONG(p+0, cursor.offset.x);
		BPLONG(p+4, cursor.offset.y);
		memmove(p+8, cursor.clr, 2*16);
		memmove(p+40, cursor.set, 2*16);
		unlock(&cursor.lk);
		return n;

	case Qmouse:
		while(mousechanged(0) == 0)
			sleep(&mouse.r, mousechanged, 0);

		lock(&screen.lk);
		if(screen.reshaped) {
			screen.reshaped = 0;
			sprint(buf, "t%11d %11d", 0, ticks());
			if(n > 1+2*12)
				n = 1+2*12;
			memmove(va, buf, n);
			unlock(&screen.lk);
			return n;
		}
		unlock(&screen.lk);

		lock(&mouse.lk);
		i = mouse.ri;
		nn = (mouse.wi + Mousequeue - i) % Mousequeue;
		if(nn < 1)
			panic("empty mouse queue");
		msec = ticks();
		while(nn > 1) {
			if(mouse.queue[i].msec + Mousewindow > msec)
				break;
			i = (i+1)%Mousequeue;
			nn--;
		}
		b = buttonmap[mouse.queue[i].buttons&7];
		/* put buttons 4 and 5 back in */
		b |= mouse.queue[i].buttons & (3<<3);
		if(scrollswap){
			if(b == 8)
				b = 16;
			else if(b == 16)
				b = 8;
		}
		sprint(buf, "m%11d %11d %11d %11d",
			mouse.queue[i].xy.x,
			mouse.queue[i].xy.y,
			b,
			mouse.queue[i].msec);
		mouse.ri = (i+1)%Mousequeue;
		unlock(&mouse.lk);
		if(n > 1+4*12)
			n = 1+4*12;
		memmove(va, buf, n);
		return n;
	}
	return 0;
}

static void
setbuttonmap(char* map)
{
	int i, x, one, two, three;

	one = two = three = 0;
	for(i = 0; i < 3; i++){
		if(map[i] == 0)
			error(Ebadarg);
		if(map[i] == '1'){
			if(one)
				error(Ebadarg);
			one = 1<<i;
		}
		else if(map[i] == '2'){
			if(two)
				error(Ebadarg);
			two = 1<<i;
		}
		else if(map[i] == '3'){
			if(three)
				error(Ebadarg);
			three = 1<<i;
		}
		else
			error(Ebadarg);
	}
	if(map[i])
		error(Ebadarg);

	memset(buttonmap, 0, 8);
	for(i = 0; i < 8; i++){
		x = 0;
		if(i & 1)
			x |= one;
		if(i & 2)
			x |= two;
		if(i & 4)
			x |= three;
		buttonmap[x] = i;
	}
}

long
mousewrite(Chan *c, void *va, long n, vlong offset)
{
	char *p;
	Point pt;
	char buf[64];
	Cmdbuf *cb;
	Cmdtab *ct;

	USED(offset);

	p = va;
	switch((long)c->qid.path){
	case Qdir:
		error(Eisdir);

	case Qcursor:
		if(n < 2*4+2*2*16){
			cursor = arrow;
			setcursor();
		}else{
			n = 2*4+2*2*16;
			lock(&cursor.lk);
			cursor.offset.x = BGLONG(p+0);
			cursor.offset.y = BGLONG(p+4);
			memmove(cursor.clr, p+8, 2*16);
			memmove(cursor.set, p+40, 2*16);
			unlock(&cursor.lk);
			setcursor();
		}
		return n;

	case Qmouse:
		if(n > sizeof buf-1)
			n = sizeof buf -1;
		memmove(buf, va, n);
		buf[n] = 0;
		p = 0;
		pt.x = strtoul(buf+1, &p, 0);
		if(p == 0)
			error(Eshort);
		pt.y = strtoul(p, 0, 0);
		if(ptinrect(pt, gscreen->r))
			mouseset(pt);
		return n;

	case Qmousectl:
		cb = parsecmd(va,n);
		if(waserror()){
			free(cb);
			nexterror();
		}
		ct = lookupcmd(cb, mousectlmsg, nelem(mousectlmsg));
		switch(ct->index){
		case CMswap:
			if(mouseswap)
				setbuttonmap("123");
			else
				setbuttonmap("321");
			mouseswap ^= 1;
			break;

		case CMscrollswap:
			scrollswap ^= 1;
			break;

		case CMbuttonmap:
			if(cb->nf==1)
				setbuttonmap("123");
			else
				setbuttonmap(cb->f[1]);
			break;
		}
		free(cb);
		poperror();
		return n;
	}

	error(Egreg);
	return -1;
}

int
mousechanged(void *a)
{
	USED(a);

	return mouse.ri != mouse.wi || screen.reshaped;
}

Dev mousedevtab = {
	'm',
	"mouse",

	devreset,
	mouseinit,
	devshutdown,
	mouseattach,
	mousewalk,
	mousestat,
	mouseopen,
	devcreate,
	mouseclose,
	mouseread,
	devbread,
	mousewrite,
	devbwrite,
	devremove,
	devwstat,
};


