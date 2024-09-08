#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

#include	<draw.h>
#include	<memdraw.h>
#include	"screen.h"

extern Memimage		*gscreen;

static Memsubfont	*memdefont;
static Memimage		*conscol;
static Memimage		*back;
static Rectangle	flushr;
static Rectangle	window;
static Point		curpos;
static int		h;

static void drawscreenputs(char*, int);
static void termscreenputs(char*, int);

static void
screenflush(void)
{
	flushmemscreen(flushr);
	flushr = Rect(10000, 10000, -10000, -10000);
}

static void
addflush(Rectangle r)
{
	if(flushr.min.x >= flushr.max.x)
		flushr = r;
	else
		combinerect(&flushr, r);
}

static void
screenwin(void)
{
	Point p;
	char *greet;
	Memimage *grey;

	back = memwhite;
	conscol = memblack;
	memfillcolor(gscreen, 0x444488FF);
	
	h = memdefont->height;

	window = insetrect(gscreen->clipr, 20);
	memimagedraw(gscreen, window, memblack, ZP, memopaque, ZP, S);
	window = insetrect(window, 4);
	memimagedraw(gscreen, window, memwhite, ZP, memopaque, ZP, S);

	/* a lot of work to get a grey color */
	grey = allocmemimage(Rect(0,0,1,1), CMAP8);
	grey->flags |= Frepl;
	grey->clipr = gscreen->clipr;
	memfillcolor(grey, 0xAAAAAAFF);
	memimagedraw(gscreen, Rect(window.min.x, window.min.y,
			window.max.x, window.min.y+h+5+6), grey, gscreen->clipr.min, nil, ZP, S);
	freememimage(grey);
	window = insetrect(window, 5);

	greet = " Plan 9 Console ";
	p = addpt(window.min, Pt(10, 0));
	memimagestring(gscreen, p, conscol, ZP, memdefont, greet);
	window.min.y += h+6;
	curpos = window.min;
	window.max.y = window.min.y+((window.max.y-window.min.y)/h)*h;
	flushmemscreen(gscreen->r);

	drawscreenputs(kmesg.buf, kmesg.n);
	screenflush();
}

static struct {
	Rectangle	r;
	Rendez		z;
	int		f;
} resize;

static int
isresized(void *arg)
{
	return resize.f != 0;
}

static void
resizeproc(void *arg)
{
	USED(arg);
	for(;;){
		sleep(&resize.z, isresized, nil);
		qlock(&drawlock);
		resize.f = 0;
		if(gscreen == nil
		|| badrect(resize.r)
		|| eqrect(resize.r, gscreen->clipr)){
			qunlock(&drawlock);
			continue;
		}
		screensize(resize.r, gscreen->chan);
		if(gscreen == nil
		|| rectclip(&resize.r, gscreen->r) == 0
		|| eqrect(resize.r, gscreen->clipr)){
			qunlock(&drawlock);
			continue;
		}
		gscreen->clipr = resize.r;

		screenwin();
		deletescreenimage();
		resetscreenimage();
		qunlock(&drawlock);
		osmsleep(1000);
	}
}

void
screenresize(Rectangle r)
{
	qlock(&drawlock);
	resize.r = r;
	resize.f = 1;
	wakeup(&resize.z);
	qunlock(&drawlock);
}

void
terminit(void)
{
	memdefont = getmemdefont();
	screenwin();
	screenputs = termscreenputs;
	kproc("resize", resizeproc, nil);
}

static void
scroll(void)
{
	int o;
	Point p;
	Rectangle r;

	o = 8*h;
	r = Rpt(window.min, Pt(window.max.x, window.max.y-o));
	p = Pt(window.min.x, window.min.y+o);
	memimagedraw(gscreen, r, gscreen, p, nil, p, S);
	r = Rpt(Pt(window.min.x, window.max.y-o), window.max);
	memimagedraw(gscreen, r, back, ZP, nil, ZP, S);
	curpos.y -= o;
}

static void
screenputc(char *buf)
{
	Point p;
	int w, pos;
	Rectangle r;
	static int *xp;
	static int xbuf[256];

	if(xp < xbuf || xp >= &xbuf[nelem(xbuf)])
		xp = xbuf;

	switch(buf[0]) {
	case '\n':
		if(curpos.y+h >= window.max.y){
			scroll();
			flushr = window;
		}
		curpos.y += h;
		screenputc("\r");
		break;
	case '\r':
		xp = xbuf;
		curpos.x = window.min.x;
		break;
	case '\t':
		p = memsubfontwidth(memdefont, " ");
		w = p.x;
		*xp++ = curpos.x;
		pos = (curpos.x-window.min.x)/w;
		pos = 8-(pos%8);
		r = Rect(curpos.x, curpos.y, curpos.x+pos*w, curpos.y + h);
		memimagedraw(gscreen, r, back, back->r.min, nil, back->r.min, S);
		addflush(r);
		curpos.x += pos*w;
		break;
	case '\b':
		if(xp <= xbuf)
			break;
		xp--;
		r = Rect(*xp, curpos.y, curpos.x, curpos.y + h);
		memimagedraw(gscreen, r, back, back->r.min, nil, back->r.min, S);
		addflush(r);
		curpos.x = *xp;
		break;
	default:
		p = memsubfontwidth(memdefont, buf);
		w = p.x;

		if(curpos.x >= window.max.x-w)
			screenputc("\n");

		*xp++ = curpos.x;
		r = Rect(curpos.x, curpos.y, curpos.x+w, curpos.y + h);
		memimagedraw(gscreen, r, back, back->r.min, nil, back->r.min, S);
		memimagestring(gscreen, curpos, conscol, ZP, memdefont, buf);
		addflush(r);
		curpos.x += w;
	}
}

static void
drawscreenputs(char *s, int n)
{
	static char rb[UTFmax+1];
	static int nrb;
	char *e;

	e = s + n;
	while(s < e){
		rb[nrb++] = *s++;
		if(nrb >= UTFmax || fullrune(rb, nrb)){
			rb[nrb] = 0;
			screenputc(rb);
			nrb = 0;
		}
	}
}

static void
termscreenputs(char *s, int n)
{
	qlock(&drawlock);
	drawscreenputs(s, n);
	screenflush();
	qunlock(&drawlock);
}
