#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include <cursor.h>
#include "screen.h"

typedef struct Cursor Cursor;

#undef	long
#undef  ulong
#define	Font		XFont
#define	Screen	XScreen
#define	Display	XDisplay
#define	Cursor	XCursor

#undef	getenv

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/IntrinsicP.h>
#include <X11/StringDefs.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include "keysym2ucs.h"

#undef	Font
#undef	Screen
#undef	Display
#undef	Cursor
#define	long	int
#define ulong	p9_ulong

/* perfect approximation to NTSC = .299r+.587g+.114b when 0 ≤ r,g,b < 256 */
#define RGB2K(r,g,b)	((156763*(r)+307758*(g)+59769*(b))>>19)

static	XDisplay*	xdisplay;	/* used holding draw lock */
static int		xtblbit;
static int 		plan9tox11[256]; /* Values for mapping between */
static int 		x11toplan9[256]; /* X11 and Plan 9 */
static	GC		xgccopy;
static	ulong		xscreenchan;
static	Drawable	xscreenid;
static	XImage*		xscreenimage;
static	Visual		*xvis;

extern char		*geometry;	/* defined in main.c */

#include "../glenda-t.xbm"

Memimage*
xallocmemimage(Rectangle r, ulong chan, int pmid, XImage **X)
{
	Memimage *m;
	XImage *xi;
	int offset;
	int d;

	m = allocmemimage(r, chan);
	if(m == nil)
		return nil;
	if(chan != GREY1 && chan != xscreenchan)
		return m;

	d = m->depth;
	if(d == 24)
		offset = r.min.x&(4-1);
	else
		offset = r.min.x&(31/d);
	r.min.x -= offset;

	assert(wordsperline(r, m->depth) <= m->width);

	xi = XCreateImage(xdisplay, xvis, m->depth==32?24:m->depth, ZPixmap, 0,
		(char*)m->data->bdata, Dx(r), Dy(r), 32, m->width*sizeof(ulong));

	if(xi == nil){
		freememimage(m);
		return nil;
	}

	/*
	 * Set the parameters of the XImage so its memory looks exactly like a
	 * Memimage, so we can call _memimagedraw on the same data.  All frame
	 * buffers we've seen, and Plan 9's graphics code, require big-endian
	 * bits within bytes, but little endian byte order within pixels.
	 */
	xi->bitmap_unit = m->depth < 8 || m->depth == 24 ? 8 : m->depth;
	xi->byte_order = LSBFirst;
	xi->bitmap_bit_order = MSBFirst;
	xi->bitmap_pad = 32;
	XInitImage(xi);
	XFlush(xdisplay);

	*X = xi;

	return m;
}

/*
 * X11 window management and kernel hooks.
 * Oh, how I loathe this code!
 */

static XColor			map[256];	/* Plan 9 colormap array */
static XColor			map7[128];	/* Plan 9 colormap array */
static uchar			map7to8[128][2];
static Colormap			xcmap;		/* Default shared colormap  */

/* for copy/paste, lifted from plan9ports */
static Atom clipboard;
static Atom utf8string;
static Atom targets;
static Atom text;
static Atom compoundtext;
static Atom wmpid;
static Atom wmdelete;

static	Drawable	xdrawable;
static	void		xexpose(XEvent*);
static	void		xresize(XEvent*);
static	void		xmouse(XEvent*);
static	void		xkeyboard(XEvent*);
static	void		xmapping(XEvent*);
static	void		xdestroy(XEvent*);
static	void		xselect(XEvent*, XDisplay*);
static	void		xproc(void*);
static	void		initmap(XDisplay*, int, Visual*);
static	GC		creategc(Drawable);
static	void		graphicscmap(XColor*);
static	int		xscreendepth;
static	XDisplay*	xkmcon;	/* used only in xproc */
static	XDisplay*	xsnarfcon;	/* used holding clip.lk */

static	int	putsnarf, assertsnarf;

Memimage *gscreen;

static int
shutup(XDisplay *d, XErrorEvent *e)
{
	char buf[200];
	iprint("X error: error code=%d, request_code=%d, minor=%d\n", e->error_code, e->request_code, e->minor_code);
	XGetErrorText(d, e->error_code, buf, sizeof(buf));
	iprint("%s\n", buf);
	USED(d);
	USED(e);
	return 0;
}

static int
panicshutup(XDisplay *d)
{
	screenputs = 0;
	panic("x error");
	return -1;
}


void
flushmemscreen(Rectangle r)
{
	int x, y;
	uchar *p;

	assert(!canqlock(&drawlock));
	if(rectclip(&r, gscreen->clipr) == 0)
		return;

	if(xtblbit && gscreen->chan == CMAP8)
		for(y=r.min.y; y<r.max.y; y++)
			for(x=r.min.x, p=byteaddr(gscreen, Pt(x,y)); x<r.max.x; x++, p++)
				*p = plan9tox11[*p];

	XPutImage(xdisplay, xscreenid, xgccopy, xscreenimage, r.min.x, r.min.y, r.min.x, r.min.y, Dx(r), Dy(r));

	if(xtblbit && gscreen->chan == CMAP8)
		for(y=r.min.y; y<r.max.y; y++)
			for(x=r.min.x, p=byteaddr(gscreen, Pt(x,y)); x<r.max.x; x++, p++)
				*p = x11toplan9[*p];

	XCopyArea(xdisplay, xscreenid, xdrawable, xgccopy, r.min.x, r.min.y, Dx(r), Dy(r), r.min.x, r.min.y);
	XFlush(xdisplay);
}

void
screeninit(void)
{
	int i, n, x, y;
	char *argv[2];
	Rectangle r;
	XWMHints hints;
	XVisualInfo xvi;
	int screen;
	XTextProperty name;
	XClassHint classhints;
	XSizeHints normalhints;
	XSetWindowAttributes attrs;
	XPixmapFormatValues *pfmt;
	Pixmap icon_pixmap;
	unsigned long pid;

	memimageinit();

	xdisplay = XOpenDisplay(NULL);
	if(xdisplay == 0)
		panic("XOpenDisplay: %r [DISPLAY=%s]", getenv("DISPLAY"));

	XSetErrorHandler(shutup);
	XSetIOErrorHandler(panicshutup);

	xkmcon = XOpenDisplay(NULL);
	if(xkmcon == 0)
		panic("XOpenDisplay: %r [DISPLAY=%s]", getenv("DISPLAY"));

	XkbSetDetectableAutoRepeat(xkmcon, True, NULL);

	clipboard = XInternAtom(xkmcon, "CLIPBOARD", False);
	utf8string = XInternAtom(xkmcon, "UTF8_STRING", False);
	targets = XInternAtom(xkmcon, "TARGETS", False);
	text = XInternAtom(xkmcon, "TEXT", False);
	compoundtext = XInternAtom(xkmcon, "COMPOUND_TEXT", False);

	xsnarfcon = XOpenDisplay(NULL);
	if(xsnarfcon == 0)
		panic("XOpenDisplay: %r [DISPLAY=%s]", getenv("DISPLAY"));

	screen = DefaultScreen(xdisplay);
	xscreendepth = DefaultDepth(xdisplay, screen);
	if(XMatchVisualInfo(xdisplay, screen, 16, TrueColor, &xvi)
	|| XMatchVisualInfo(xdisplay, screen, 16, DirectColor, &xvi)){
		xvis = xvi.visual;
		xscreendepth = 16;
		xtblbit = 1;
	}
	else if(XMatchVisualInfo(xdisplay, screen, 24, TrueColor, &xvi)
	|| XMatchVisualInfo(xdisplay, screen, 24, DirectColor, &xvi)){
		xvis = xvi.visual;
		xscreendepth = 24;
		xtblbit = 1;
	}
	else if(XMatchVisualInfo(xdisplay, screen, 8, PseudoColor, &xvi)
	|| XMatchVisualInfo(xdisplay, screen, 8, StaticColor, &xvi)){
		if(xscreendepth > 8)
			panic("can't deal with colormapped depth %d screens", xscreendepth);
		xvis = xvi.visual;
		xscreendepth = 8;
	}
	else{
		if(xscreendepth != 8)
			panic("can't deal with depth %d screens", xscreendepth);
		xvis = DefaultVisual(xdisplay, screen);
	}

	/*
	 * xscreendepth is only the number of significant pixel bits,
	 * not the total.  We need to walk the display list to find
	 * how many actual bits are being used per pixel.
	 */
	xscreenchan = 0; /* not a valid channel */
	pfmt = XListPixmapFormats(xdisplay, &n);
	for(i=0; i<n; i++){
		if(pfmt[i].depth == xscreendepth){
			switch(pfmt[i].bits_per_pixel){
			case 1:	/* untested */
				xscreenchan = GREY1;
				break;
			case 2:	/* untested */
				xscreenchan = GREY2;
				break;
			case 4:	/* untested */
				xscreenchan = GREY4;
				break;
			case 8:
				xscreenchan = CMAP8;
				break;
			case 16: /* uses 16 rather than 15, empirically. */
				xscreenchan = RGB16;
				break;
			case 24: /* untested (impossible?) */
				xscreenchan = RGB24;
				break;
			case 32:
				xscreenchan = CHAN4(CIgnore, 8, CRed, 8, CGreen, 8, CBlue, 8);
				break;
			}
		}
	}
	if(xscreenchan == 0)
		panic("unknown screen pixel format");

	initmap(xdisplay, screen, xvis);

	x = y = 0;
	r = ZR;
	if(geometry != nil)
		XParseGeometry(geometry, &x, &y, (unsigned int*)&r.max.x, (unsigned int*)&r.max.y);

	if(r.max.x == 0)
		r.max.x = WidthOfScreen(ScreenOfDisplay(xdisplay, screen))*3/4;
	if(r.max.y == 0)
		r.max.y = HeightOfScreen(ScreenOfDisplay(xdisplay, screen))*3/4;

	attrs.colormap = xcmap;
	attrs.background_pixel = 0;
	attrs.border_pixel = 0;
	/* attrs.override_redirect = 1;*/ /* WM leave me alone! |CWOverrideRedirect */
	xdrawable = XCreateWindow(xkmcon, RootWindow(xdisplay, screen), x, y, Dx(r), Dy(r), 0,
		xscreendepth, InputOutput, xvis, CWBackPixel|CWBorderPixel|CWColormap, &attrs);

	/* load the given bitmap data and create an X pixmap containing it. */
	icon_pixmap = XCreateBitmapFromData(xkmcon, RootWindow(xdisplay, screen),
		(char *)glenda_t_bits, glenda_t_width, glenda_t_height);

	/*
	 * set up property as required by ICCCM
	 */
	if((name.value = (uchar*)getenv("WM_NAME")) == nil)
		name.value = (uchar*)"drawterm";
	name.encoding = XA_STRING;
	name.format = 8;
	name.nitems = strlen((char*)name.value);
	normalhints.flags = USSize;
	normalhints.width = Dx(r);
	normalhints.height = Dy(r);
	hints.flags = IconPixmapHint |InputHint|StateHint;
	hints.input = 1;
	hints.initial_state = NormalState;
	hints.icon_pixmap = icon_pixmap;

	classhints.res_name = "drawterm";
	if((classhints.res_class = getenv("WM_CLASS")) == nil)
		classhints.res_class = "Drawterm";
	argv[0] = "drawterm";
	argv[1] = nil;
	XSetWMProperties(xkmcon, xdrawable,
		&name,			/* XA_WM_NAME property for ICCCM */
		&name,			/* XA_WM_ICON_NAME */
		argv,			/* XA_WM_COMMAND */
		1,			/* argc */
		&normalhints,		/* XA_WM_NORMAL_HINTS */
		&hints,			/* XA_WM_HINTS */
		&classhints);		/* XA_WM_CLASS */
	XFlush(xkmcon);
	if ((wmpid = XInternAtom(xdisplay, "_NET_WM_PID", False)) != None) {
		pid = (unsigned long) getpid();
		XChangeProperty(xkmcon, xdrawable,
			wmpid, /* Atom property */
			XA_CARDINAL, /* Atom type */
			32, /* int format, 32 really is "long" */
			PropModeReplace, /* int mode */
			(uchar *)&pid, /* unsigned char * data */
			1); /* int nelements */
		XFlush(xkmcon);
	}

	/*
	 * put the window on the screen
	 */
	wmdelete = XInternAtom(xkmcon, "WM_DELETE_WINDOW", True);
	XSetWMProtocols(xkmcon, xdrawable, &wmdelete, 1);
	XMapWindow(xkmcon, xdrawable);
	XFlush(xkmcon);

	screensize(r, xscreenchan);
	if(gscreen == nil)
		panic("screensize failed");

	gscreen->clipr = r;
	kproc("xscreen", xproc, nil);

	qlock(&drawlock);
	terminit();
	flushmemscreen(gscreen->clipr);
	qunlock(&drawlock);
}

void
screensize(Rectangle r, ulong chan)
{
	Drawable pix;
	Memimage *mi;
	XImage *xi;
	GC gc;

	pix = XCreatePixmap(xdisplay, xdrawable, Dx(r), Dy(r), xscreendepth);
	if(pix == 0)
		return;

	gc = creategc(pix);
	if(gc == NULL){
		XFreePixmap(xdisplay, pix);
		return;
	}

	mi = xallocmemimage(r, chan, pix, &xi);
	if(mi == nil){
		XFreeGC(xdisplay, gc);
		XFreePixmap(xdisplay, pix);
		return;
	}

	if(gscreen != nil){
		xscreenimage->data = NULL;	/* free'd by freememimage() */
		XDestroyImage(xscreenimage);
		freememimage(gscreen);

		XFreeGC(xdisplay, xgccopy);
		XFreePixmap(xdisplay, xscreenid);
	}

	xscreenimage = xi;
	xscreenid = pix;
	xgccopy = gc;

	gscreen = mi;
	gscreen->clipr = ZR;
}

Memdata*
attachscreen(Rectangle *r, ulong *chan, int *depth, int *width, int *softscreen)
{
	*r = gscreen->clipr;
	*chan = gscreen->chan;
	*depth = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;

	gscreen->data->ref++;
	return gscreen->data;
}

static int
revbyte(int b)
{
	int r;

	r = 0;
	r |= (b&0x01) << 7;
	r |= (b&0x02) << 5;
	r |= (b&0x04) << 3;
	r |= (b&0x08) << 1;
	r |= (b&0x10) >> 1;
	r |= (b&0x20) >> 3;
	r |= (b&0x40) >> 5;
	r |= (b&0x80) >> 7;
	return r;
}

void
mouseset(Point xy)
{
	qlock(&drawlock);
	XWarpPointer(xdisplay, None, xdrawable, 0, 0, 0, 0, xy.x, xy.y);
	XFlush(xdisplay);
	qunlock(&drawlock);
}

static XCursor xcursor;

void
setcursor(void)
{
	XCursor xc;
	XColor fg, bg;
	Pixmap xsrc, xmask;
	int i;
	uchar src[2*16], mask[2*16];

	for(i=0; i<2*16; i++){
		src[i] = revbyte(cursor.set[i]);
		mask[i] = revbyte(cursor.set[i] | cursor.clr[i]);
	}

	qlock(&drawlock);
	fg = map[0];
	bg = map[255];
	xsrc = XCreateBitmapFromData(xdisplay, xdrawable, (char*)src, 16, 16);
	xmask = XCreateBitmapFromData(xdisplay, xdrawable, (char*)mask, 16, 16);
	xc = XCreatePixmapCursor(xdisplay, xsrc, xmask, &fg, &bg, -cursor.offset.x, -cursor.offset.y);
	if(xc != 0) {
		XDefineCursor(xdisplay, xdrawable, xc);
		if(xcursor != 0)
			XFreeCursor(xdisplay, xcursor);
		xcursor = xc;
	}
	XFreePixmap(xdisplay, xsrc);
	XFreePixmap(xdisplay, xmask);
	XFlush(xdisplay);
	qunlock(&drawlock);
}

void
cursorarrow(void)
{
	qlock(&drawlock);
	if(xcursor != 0){
		XFreeCursor(xdisplay, xcursor);
		xcursor = 0;
	}
	XUndefineCursor(xdisplay, xdrawable);
	XFlush(xdisplay);
	qunlock(&drawlock);
}

static void
xproc(void *arg)
{
	ulong mask;
	XEvent event;

	mask = 	KeyPressMask|
		KeyReleaseMask|
		ButtonPressMask|
		ButtonReleaseMask|
		PointerMotionMask|
		Button1MotionMask|
		Button2MotionMask|
		Button3MotionMask|
		Button4MotionMask|
		Button5MotionMask|
		ExposureMask|
		EnterWindowMask|
		LeaveWindowMask|
		FocusChangeMask|
		StructureNotifyMask;

	XSelectInput(xkmcon, xdrawable, mask);
	for(;;) {
		XNextEvent(xkmcon, &event);
		xselect(&event, xkmcon);
		xkeyboard(&event);
		xmouse(&event);
		xresize(&event);
		xexpose(&event);
		xmapping(&event);
		xdestroy(&event);
	}
}

static void
graphicscmap(XColor *map)
{
	int r, g, b, cr, cg, cb, v, num, den, idx, v7, idx7;

	for(r=0; r!=4; r++) {
		for(g = 0; g != 4; g++) {
			for(b = 0; b!=4; b++) {
				for(v = 0; v!=4; v++) {
					den=r;
					if(g > den)
						den=g;
					if(b > den)
						den=b;
					/* divide check -- pick grey shades */
					if(den==0)
						cr=cg=cb=v*17;
					else {
						num=17*(4*den+v);
						cr=r*num/den;
						cg=g*num/den;
						cb=b*num/den;
					}
					idx = r*64 + v*16 + ((g*4 + b + v - r) & 15);
					map[idx].red = cr*0x0101;
					map[idx].green = cg*0x0101;
					map[idx].blue = cb*0x0101;
					map[idx].pixel = idx;
					map[idx].flags = DoRed|DoGreen|DoBlue;

					v7 = v >> 1;
					idx7 = r*32 + v7*16 + g*4 + b;
					if((v & 1) == v7){
						map7to8[idx7][0] = idx;
						if(den == 0) { 		/* divide check -- pick grey shades */
							cr = ((255.0/7.0)*v7)+0.5;
							cg = cr;
							cb = cr;
						}
						else {
							num=17*15*(4*den+v7*2)/14;
							cr=r*num/den;
							cg=g*num/den;
							cb=b*num/den;
						}
						map7[idx7].red = cr*0x0101;
						map7[idx7].green = cg*0x0101;
						map7[idx7].blue = cb*0x0101;
						map7[idx7].pixel = idx7;
						map7[idx7].flags = DoRed|DoGreen|DoBlue;
					}
					else
						map7to8[idx7][1] = idx;
				}
			}
		}
	}
}

/*
 * Initialize and install the drawterm colormap as a private colormap for this
 * application.  Drawterm gets the best colors here when it has the cursor focus.
 */
static void
initmap(XDisplay *xdisplay, int screen, Visual *xvis)
{
	XColor c;
	int i;
	ulong p, pp;
	char buf[30];

	xcmap = DefaultColormap(xdisplay, screen);
	if(xvis->class == StaticColor)
		return;

	graphicscmap(map);
	if(xscreendepth <= 1)
		return;

	if(xscreendepth >= 24) {
		/* The pixel value returned from XGetPixel needs to
		 * be converted to RGB so we can call rgb2cmap()
		 * to translate between 24 bit X and our color. Unfortunately,
		 * the return value appears to be display server endian
		 * dependant. Therefore, we run some heuristics to later
		 * determine how to mask the int value correctly.
		 * Yeah, I know we can look at xvis->byte_order but
		 * some displays say MSB even though they run on LSB.
		 * Besides, this is more anal.
		 */
		if(xvis != DefaultVisual(xdisplay, screen))
			xcmap = XCreateColormap(xdisplay, RootWindow(xdisplay, screen), xvis, AllocNone);
		c = map[19];
		/* find out index into colormap for our RGB */
		if(!XAllocColor(xdisplay, xcmap, &c))
			panic("screen-x11 can't alloc color");

		p  = c.pixel;
		pp = rgb2cmap((p>>16)&0xff,(p>>8)&0xff,p&0xff);
		if(pp!=map[19].pixel) {
			/* check if endian is other way */
			pp = rgb2cmap(p&0xff,(p>>8)&0xff,(p>>16)&0xff);
			if(pp!=map[19].pixel)
				panic("cannot detect x server byte order");
			switch(xscreenchan){
			case RGB24:
				xscreenchan = BGR24;
				break;
			case XRGB32:
				xscreenchan = XBGR32;
				break;
			default:
				panic("don't know how to byteswap channel %s",
					chantostr(buf, xscreenchan));
				break;
			}
		}
	} else if(xvis->class == TrueColor || xvis->class == DirectColor) {
	} else if(xvis->class == PseudoColor) {
		if(xtblbit == 0){
			xcmap = XCreateColormap(xdisplay, RootWindow(xdisplay, screen), xvis, AllocAll);
			XStoreColors(xdisplay, xcmap, map, 256);
			for(i = 0; i < 256; i++) {
				plan9tox11[i] = i;
				x11toplan9[i] = i;
			}
		}
		else {
			for(i = 0; i < 128; i++) {
				c = map7[i];
				if(!XAllocColor(xdisplay, xcmap, &c))
					panic("can't alloc colors in default map, don't use -7");
				plan9tox11[map7to8[i][0]] = c.pixel;
				plan9tox11[map7to8[i][1]] = c.pixel;
				x11toplan9[c.pixel] = map7to8[i][0];
			}
		}
	}
	else
		panic("unsupported visual class %d", xvis->class);
}

static void
xdestroy(XEvent *e)
{
	XDestroyWindowEvent *xe;
	XClientMessageEvent *ce;

	switch(e->type){
	case ClientMessage:
		/* Handle WM_DELETE_WINDOW */
		ce = (XClientMessageEvent*)e;
		if(ce->window == xdrawable && ce->data.l[0] == wmdelete)
			exit(0);
	case DestroyNotify:
		xe = (XDestroyWindowEvent*)e;
		if(xe->window == xdrawable)
			exit(0);
	}
}

static void
xresize(XEvent *e)
{
	if(e->type != ConfigureNotify)
		return;
	screenresize(Rect(0, 0, ((XConfigureEvent*)e)->width, ((XConfigureEvent*)e)->height));
}

static void
xmapping(XEvent *e)
{
	XMappingEvent *xe;

	if(e->type != MappingNotify)
		return;
	xe = (XMappingEvent*)e;
	USED(xe);
}


/*
 * Disable generation of GraphicsExpose/NoExpose events in the GC.
 */
static GC
creategc(Drawable d)
{
	XGCValues gcv;

	gcv.function = GXcopy;
	gcv.graphics_exposures = False;
	return XCreateGC(xdisplay, d, GCFunction|GCGraphicsExposures, &gcv);
}

static void
xexpose(XEvent *e)
{
	Rectangle r;
	XExposeEvent *xe;

	if(e->type != Expose)
		return;
	xe = (XExposeEvent*)e;
	r.min.x = xe->x;
	r.min.y = xe->y;
	r.max.x = xe->x + xe->width;
	r.max.y = xe->y + xe->height;

	qlock(&drawlock);
	flushmemscreen(r);
	qunlock(&drawlock);
}

static void
xkeyboard(XEvent *e)
{
	static int altdown;
	static int shiftdown;
	static int superdown;
	KeySym k;

	switch(e->xany.type){
	case KeyPress:
	case KeyRelease:
		break;
	case FocusIn:
	case FocusOut:
		if(altdown){
			altdown = 0;
			kbdkey(Kalt, 0);
			kbdkey(Kalt, 1);
			kbdkey(Kalt, 0);
		}
		if(shiftdown){
			shiftdown = 0;
			kbdkey(Kshift, 0);
		}
		if(superdown){
			superdown = 0;
			kbdkey(Kmod4, 0);
		}
		/* wet floor */
	default:
		return;
	}

	XLookupString((XKeyEvent*)e, NULL, 0, &k, NULL);

	if(k == XK_Multi_key || k == NoSymbol)
		return;
	if(k&0xFF00){
		switch(k){
		case XK_BackSpace:
		case XK_Tab:
		case XK_Escape:
		case XK_Delete:
		case XK_KP_0:
		case XK_KP_1:
		case XK_KP_2:
		case XK_KP_3:
		case XK_KP_4:
		case XK_KP_5:
		case XK_KP_6:
		case XK_KP_7:
		case XK_KP_8:
		case XK_KP_9:
		case XK_KP_Divide:
		case XK_KP_Multiply:
		case XK_KP_Subtract:
		case XK_KP_Add:
		case XK_KP_Decimal:
			k &= 0x7F;
			break;
		case XK_Linefeed:
			k = '\r';
			break;
		case XK_KP_Space:
			k = ' ';
			break;
		case XK_Home:
		case XK_KP_Home:
			k = Khome;
			break;
		case XK_Left:
		case XK_KP_Left:
			k = Kleft;
			break;
		case XK_Up:
		case XK_KP_Up:
			k = Kup;
			break;
		case XK_Down:
		case XK_KP_Down:
			k = Kdown;
			break;
		case XK_Right:
		case XK_KP_Right:
			k = Kright;
			break;
		case XK_Page_Down:
		case XK_KP_Page_Down:
			k = Kpgdown;
			break;
		case XK_End:
		case XK_KP_End:
			k = Kend;
			break;
		case XK_Page_Up:
		case XK_KP_Page_Up:
			k = Kpgup;
			break;
		case XK_Insert:
		case XK_KP_Insert:
			k = Kins;
			break;
		case XK_KP_Enter:
		case XK_Return:
			k = '\n';
			break;
		case XK_Alt_L:
		case XK_Alt_R:
			k = Kalt;
			break;
		case XK_Super_L:
		case XK_Super_R:
			k = Kmod4;
			break;

		case XK_Shift_L:
		case XK_Shift_R:
			k = Kshift;
			break;
		case XK_Control_L:
		case XK_Control_R:
			k = Kctl;
			break;
		case XK_Shift_Lock:
		case XK_Caps_Lock:
			k = Kcaps;
			break;
		case XK_Scroll_Lock:
			k = Kscroll;
			break;

		case XK_F1:
		case XK_F2:
		case XK_F3:
		case XK_F4:
		case XK_F5:
		case XK_F6:
		case XK_F7:
		case XK_F8:
		case XK_F9:
		case XK_F10:
		case XK_F11:
		case XK_F12:
			k = KF|(k - XK_F1 + 1);
			break;

		case XK_Meta_L:
		case XK_Meta_R:
		case XK_Hyper_L:
		case XK_Hyper_R:
			return;
		default:		/* not ISO-1 or tty control */
  			if(k>0xff){
				k = keysym2ucs(k); /* supplied by X */
				if(k == -1)
					return;
			}
			break;
		}
	}

	/* Compensate for servers that call a minus a hyphen */
	if(k == XK_hyphen)
		k = XK_minus;
	/* Do control mapping ourselves if translator doesn't */
	if(e->xkey.state&ControlMask && k != Kalt && k != Kctl)
		if(k < 0x80)
			k &= 0x1f;
	if(k == NoSymbol)
		return;
	altdown = e->xany.type == KeyPress && k == Kalt;
	shiftdown = e->xany.type == KeyPress && k == Kshift;
	superdown = e->xany.type == KeyPress && k == Kmod4;
	kbdkey(k, e->xany.type == KeyPress);
}

static void
xmouse(XEvent *e)
{
	Mousestate ms;
	XButtonEvent *be;
	XMotionEvent *me;
	int s;

	if(putsnarf != assertsnarf){
		assertsnarf = putsnarf;
		XSetSelectionOwner(xkmcon, XA_PRIMARY, xdrawable, CurrentTime);
		if(clipboard != None)
			XSetSelectionOwner(xkmcon, clipboard, xdrawable, CurrentTime);
		XFlush(xkmcon);
	}

	switch(e->type){
	case ButtonPress:
		be = (XButtonEvent *)e;
		/*
		 * Fake message, just sent to make us announce snarf.
		 * Apparently state and button are 16 and 8 bits on
		 * the wire, since they are truncated by the time they
		 * get to us.
		 */
		if(be->send_event
		&& (~be->state&0xFFFF)==0
		&& (~be->button&0xFF)==0)
			return;
		ms.xy.x = be->x;
		ms.xy.y = be->y;
		s = be->state;
		ms.msec = be->time;
		switch(be->button){
		case 1:
			s |= Button1Mask;
			break;
		case 2:
			s |= Button2Mask;
			break;
		case 3:
			s |= Button3Mask;
			break;
		case 4:
			s |= Button4Mask;
			break;
		case 5:
			s |= Button5Mask;
			break;
		}
		break;
	case ButtonRelease:
		be = (XButtonEvent *)e;
		ms.xy.x = be->x;
		ms.xy.y = be->y;
		ms.msec = be->time;
		s = be->state;
		switch(be->button){
		case 1:
			s &= ~Button1Mask;
			break;
		case 2:
			s &= ~Button2Mask;
			break;
		case 3:
			s &= ~Button3Mask;
			break;
		case 4:
			s &= ~Button4Mask;
			break;
		case 5:
			s &= ~Button5Mask;
			break;
		}
		break;
	case MotionNotify:
		me = (XMotionEvent *)e;
		s = me->state;
		ms.xy.x = me->x;
		ms.xy.y = me->y;
		ms.msec = me->time;
		break;
	default:
		return;
	}

	ms.buttons = 0;
	if(s & Button1Mask)
		ms.buttons |= 1;
	if(s & Button2Mask)
		ms.buttons |= 2;
	if(s & Button3Mask)
		ms.buttons |= 4;
	if(s & Button4Mask)
		ms.buttons |= 8;
	if(s & Button5Mask)
		ms.buttons |= 16;

	absmousetrack(ms.xy.x, ms.xy.y, ms.buttons, ms.msec);
}

void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
	ulong v;

	v = cmap2rgb(i);
	*r = (v>>16)&0xFF;
	*g = (v>>8)&0xFF;
	*b = v&0xFF;
}

void
setcolor(ulong i, ulong r, ulong g, ulong b)
{
	/* no-op */
}

/*
 * Cut and paste.  Just couldn't stand to make this simple...
 */

typedef struct Clip Clip;
struct Clip
{
	char buf[SnarfSize];
	QLock lk;
};
Clip clip;

#undef long	/* sic */
#undef ulong

static char*
_xgetsnarf(XDisplay *xd)
{
	uchar *data, *xdata;
	Atom selection, type, prop;
	unsigned long lastlen;
	unsigned long dummy, len;
	int fmt, i;
	Window w;

	qlock(&clip.lk);
	/*
	 * Have we snarfed recently and the X server hasn't caught up?
	 */
	if(putsnarf != assertsnarf)
		goto mine;

	/*
	 * Is there a primary selection (highlighted text in an xterm)?
	 */
	selection = XA_PRIMARY;
	w = XGetSelectionOwner(xd, XA_PRIMARY);
	if(w == xdrawable){
	mine:
		data = (uchar*)strdup(clip.buf);
		goto out;
	}

	/*
	 * If not, is there a clipboard selection?
	 */
	if(w == None && selection != None){
		selection = clipboard;
		w = XGetSelectionOwner(xd, selection);
		if(w == xdrawable)
			goto mine;
	}

	/*
	 * If not, give up.
	 */
	if(w == None){
		data = nil;
		goto out;
	}

	/*
	 * We should be waiting for SelectionNotify here, but it might never
	 * come, and we have no way to time out.  Instead, we will clear
	 * local property #1, request our buddy to fill it in for us, and poll
	 * until he's done or we get tired of waiting.
	 */
	prop = 1;
	XChangeProperty(xd, xdrawable, prop, utf8string, 8, PropModeReplace, (uchar*)"", 0);
	XConvertSelection(xd, selection, utf8string, prop, xdrawable, CurrentTime);
	XFlush(xd);
	lastlen = 0;
	for(i=0; i<10 || (lastlen!=0 && i<30); i++){
		usleep(100*1000);
		XGetWindowProperty(xd, xdrawable, prop, 0, 0, 0, AnyPropertyType,
			&type, &fmt, &dummy, &len, &data);
		if(lastlen == len && len > 0)
			break;
		lastlen = len;
	}
	if(i == 10){
		data = nil;
		goto out;
	}
	/* get the property */
	data = nil;
	XGetWindowProperty(xd, xdrawable, prop, 0, SnarfSize/sizeof(unsigned long), 0,
		AnyPropertyType, &type, &fmt, &len, &dummy, &xdata);
	if((type != XA_STRING && type != utf8string) || len == 0){
		if(xdata)
			XFree(xdata);
		data = nil;
	}else{
		if(xdata){
			data = (uchar*)strdup((char*)xdata);
			XFree(xdata);
		}else
			data = nil;
	}
out:
	qunlock(&clip.lk);
	return (char*)data;
}

static void
_xputsnarf(XDisplay *xd, char *data)
{
	XButtonEvent e;

	if(strlen(data) >= SnarfSize)
		return;
	qlock(&clip.lk);
	strcpy(clip.buf, data);

	/* leave note for mouse proc to assert selection ownership */
	putsnarf++;

	/* send mouse a fake event so snarf is announced */
	memset(&e, 0, sizeof e);
	e.type = ButtonPress;
	e.window = xdrawable;
	e.state = ~0;
	e.button = ~0;
	XSendEvent(xd, xdrawable, True, ButtonPressMask, (XEvent*)&e);
	XFlush(xd);
	qunlock(&clip.lk);
}

static void
xselect(XEvent *e, XDisplay *xd)
{
	char *name;
	XEvent r;
	XSelectionRequestEvent *xe;
	Atom a[4];

	if(e->xany.type != SelectionRequest)
		return;

	memset(&r, 0, sizeof r);
	xe = (XSelectionRequestEvent*)e;
if(0) iprint("xselect target=%d requestor=%d property=%d selection=%d\n",
	xe->target, xe->requestor, xe->property, xe->selection);
	r.xselection.property = xe->property;
	if(xe->target == targets){
		a[0] = XA_STRING;
		a[1] = utf8string;
		a[2] = text;
		a[3] = compoundtext;

		XChangeProperty(xd, xe->requestor, xe->property, XA_ATOM,
			32, PropModeReplace, (uchar*)a, sizeof a);
	}else if(xe->target == XA_STRING || xe->target == utf8string || xe->target == text || xe->target == compoundtext){
	text:
		/* if the target is STRING we're supposed to reply with Latin1 XXX */
		qlock(&clip.lk);
		XChangeProperty(xd, xe->requestor, xe->property, xe->target,
			8, PropModeReplace, (uchar*)clip.buf, strlen(clip.buf));
		qunlock(&clip.lk);
	}else{
		name = XGetAtomName(xd, xe->target);
		if(name == nil)
			iprint("XGetAtomName %d failed\n", xe->target);
		if(name){
			if(strcmp(name, "TIMESTAMP") == 0){
				/* nothing */
			}else if(strncasecmp(name, "image/", 6) == 0){
				/* nothing */
			}else if(strcasecmp(name, "text/html") == 0){
				/* nothing */
			}else if(strcasecmp(name, "text/plain") == 0 || strcasecmp(name, "text/plain;charset=UTF-8") == 0){
				goto text;
			}else if(0)
				iprint("cannot handle selection request for '%s' (%d)\n", name, (int)xe->target);
		}
		r.xselection.property = None;
	}

	r.xselection.display = xe->display;
	/* r.xselection.property filled above */
	r.xselection.target = xe->target;
	r.xselection.type = SelectionNotify;
	r.xselection.requestor = xe->requestor;
	r.xselection.time = xe->time;
	r.xselection.send_event = True;
	r.xselection.selection = xe->selection;
	XSendEvent(xd, xe->requestor, False, 0, &r);
	XFlush(xd);
}

char*
clipread(void)
{
	return _xgetsnarf(xsnarfcon);
}

int
clipwrite(char *buf)
{
	_xputsnarf(xsnarfcon, buf);
	return 0;
}

void
guimain(void)
{
	cpubody();
}
