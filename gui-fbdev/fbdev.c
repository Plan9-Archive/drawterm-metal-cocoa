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

#undef long
#undef ulong

#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/kdev_t.h>
#include <linux/keyboard.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>

#include <stdatomic.h>

#define ulong p9_ulong

Memimage *gscreen;

static Rectangle	update_rect;
static Rectangle	screenr;
static int	startmode;
static int	altdown;
static uchar	*fbp;
static Memimage	*screenimage;
static Memimage	*backbuf;
static int	linelength;
static Point	mousexy;
static ulong	chan;
static int	depth;
static _Atomic int	dirty;
static _Atomic int	switchaway;
static Rendez	rendezflush;
static QLock	flushlock;
static char	*snarfbuf;

static Memimage* fbattach(int fbdevidx);
static int onevent(struct input_event*);
static void termctl(uint32_t o, int or);
static void ttyswitch(int sig);
static void consinit(void);
static void consfinal(void);
static void consfinalsig(int sig);
static int needflush(void *);
static void ignore0(void *);
static void unblank(void *);

static int
needflush(void *v)
{
	return atomic_load(&dirty);
}

static void
_fbput(Memimage *m, Rectangle r) {
	int dx, xoffset, ylen;
	void *ptr, *p, *max_row;

	ylen = 4 * m->width;
	dx = depth * Dx(r);
	xoffset = depth * r.min.x;
	p = fbp + xoffset + r.min.y * linelength;
	ptr = m->data->bdata + xoffset + r.min.y * ylen;
	max_row = m->data->bdata + r.max.y * ylen;
	while(ptr < max_row){
		memcpy(p, ptr, dx);
		p += linelength;
		ptr += ylen;
	}
}

static Memimage*
fbattach(int fbdevidx)
{
	Rectangle r;
	char devname[64];
	size_t size;
	int fd;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;

	/*
	 * Connect to /dev/fb0
	 */
	snprintf(devname, sizeof(devname) - 1, "/dev/fb%d", fbdevidx);
	if ((fd = open(devname, O_RDWR | O_CLOEXEC)) < 0)
		goto err;

	if (ioctl(fd, FBIOGET_VSCREENINFO, &(vinfo)) < 0)
		goto err;

	switch (vinfo.bits_per_pixel) {
	case 32:
		chan = XRGB32;
		depth = 4;
		break;
	case 16:
		chan = RGB16;
		depth = 2;
		break;
	default:
		goto err;
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &(finfo)) < 0)
		goto err;
	linelength = finfo.line_length;

	size = vinfo.yres_virtual * linelength;
	if ((fbp = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0)) < 0)
		goto err;
	/*
	 * Figure out underlying screen format.
	 */
	r = Rect(0, 0, vinfo.xres_virtual, vinfo.yres_virtual);

	screenr = r;

	screenimage = allocmemimage(r, chan);
	backbuf = allocmemimage(r, chan);
	return backbuf;

err:
	return nil;
}

static void
eventattach(struct pollfd **pfd, int *nstart)
{
	char eventfile[PATH_MAX] = "/dev/input/event";
	char buf[1024], *pbuf, *end;
	struct pollfd *p;
	int n, m;

	p = *pfd;
	n = *nstart;
	pbuf = buf;

	while(read(p->fd, buf, sizeof(buf)) > 0 || errno == EINTR)
		;	// read left over
	lseek(p->fd, 0, SEEK_SET);
	while((m = read(p->fd, pbuf, buf+sizeof(buf)-1-pbuf)) > 0 || errno == EINTR){
		if(m<=0)
			continue;
		pbuf[m] = '\0';
		pbuf = buf;
		do{
			end = strchr(pbuf, '\n');
			if(!end){
				if(buf==pbuf)
					panic("event line too long");
				for(m=0; pbuf[m]; ++m)
					buf[m] = pbuf[m];
				pbuf = buf + m;
				break;
			}
			if(*pbuf == 'H'){
				for(++pbuf; strncmp(pbuf, "event", 5) && pbuf < end; ++pbuf);
				if(pbuf == end){
					++pbuf;
					continue;
				}
				pbuf += 5;	// event
				m = strcspn(pbuf, " \n");
				pbuf[m] = '\0';
				if(m < sizeof(eventfile)-17)	// skip "/dev/input/event"
					strcpy(eventfile+16, pbuf);
				++n;
				p = realloc(p, n * sizeof(struct pollfd));
				if(!p)
					panic("realloc pollfd: %r");
				p[n-1].fd = open(eventfile, O_RDONLY|O_CLOEXEC);
				p[n-1].events = POLLIN;
				if (p[n-1].fd < 0)
					n--;
			}
			pbuf = end + 1;
		}while(*pbuf);
	}
	if(n <= *nstart)
		panic("no input events: %r");

	*pfd = p;
	*nstart = n;
}

static void
eventdetach(struct pollfd pfd[], int npfd)
{
	int n;

	for(n = 0; n < npfd; ++n)
		close(pfd[n].fd);
}

void
flushmemscreen(Rectangle r)
{
	if (rectclip(&r, screenimage->r) == 0)
		return;
	if (Dx(r) == 0 || Dy(r) == 0)
		return;

	assert(!canqlock(&drawlock));

	if (Dx(update_rect) == 0 || Dy(update_rect) == 0)
		update_rect = r;
	else
		combinerect(&update_rect, r);
	atomic_store(&dirty, 1);
	wakeup(&rendezflush);
}

static void
fbflush(void *v)
{
	Rectangle r;
	int x, y, i;
	Point p;
	long fbloc;
	int x2, y2;
	ulong del;
	ulong ms;

	del = 16;

	for(;;){
		ksleep(&rendezflush, needflush, 0);
		qlock(&flushlock);

		ms = ticks();

		qlock(&drawlock);
		r = update_rect;
		update_rect = Rect(0,0,0,0);
		memimagedraw(screenimage, r, backbuf, r.min, nil, r.min, S);
		atomic_store(&dirty, 0);
		qunlock(&drawlock);

		p = mousexy;

		// draw cursor
		for (x = 0; x < 16; x++) {
			x2 = x + cursor.offset.x;
			if ((p.x + x2) < 0)
				continue;
			if ((p.x + x2) >= screenimage->r.max.x)
				break;
			for (y = 0; y < 16; y++) {
				y2 = y + cursor.offset.y;
				if ((p.y + y2) < 0)
					continue;
				if ((p.y + y2) >= screenimage->r.max.y)
					break;
				i = y * 2 + x / 8;
				fbloc = ((p.y+y2) * screenimage->r.max.x + (p.x+x2)) * depth;
				if (cursor.clr[i] & (128 >> (x % 8))) {
					switch (depth) {
					case 2:
						*((uint16_t*)(screenimage->data->bdata + fbloc)) = 0xFFFF;
						break;
					case 4:
						*((uint32_t*)(screenimage->data->bdata + fbloc)) = 0xFFFFFFFF;
						break;
					}
				}
				if (cursor.set[i] & (128 >> (x % 8))) {
					switch (depth) {
					case 2:
						*((uint16_t*)(screenimage->data->bdata + fbloc)) = 0x0000;
						break;
					case 4:
						*((uint32_t*)(screenimage->data->bdata + fbloc)) = 0xFF000000;
						break;
					}
				}
			}
		}
		_fbput(screenimage, r);
		qunlock(&flushlock);

		ms = ms + del - ticks();
		if(ms > 0 && ms < del)
			osmsleep(ms);
	}
}

static void
ignore0(void *)
{
	int n;
	char buf[32];

	while((n = read(0, buf, sizeof buf)) > 0 || errno == EINTR)
		if (n <= 0 && errno != EINTR)
			panic("read 0: %r");
}

static void
unblank(void *)
{
	for(;;){
		struct vt_event e = {.event = VT_EVENT_UNBLANK};
		if(!ioctl(0, VT_WAITEVENT, &e)){
			qlock(&drawlock);
			flushmemscreen(gscreen->clipr);
			qunlock(&drawlock);
		}
	}
}

static void
consinit(void)
{
	struct vt_mode vm;

	if(ioctl(0, KDGETMODE, &startmode) < 0)
		panic("ioctl KDGETMODE: %r");
	write(0, "\e[9;30]", 7);	// blank time 30 min
	write(0, "\e[?25l", 6);		// hide cursor
	termctl(~(ICANON|ECHO), 0);
	signal(SIGUSR1, ttyswitch);
	signal(SIGINT, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTERM, consfinalsig);

	vm.mode = VT_PROCESS;
	vm.waitv = 0;
	vm.relsig = SIGUSR1;
	vm.acqsig = 0;
	vm.frsig = 0;
	if(ioctl(0, VT_SETMODE, &vm) < 0)
		panic("ioctl VT_SETMODE: %r");
}

static void
consfinal(void)
{
	write(0, "\e[?25h", 6);
	if (startmode == KD_TEXT)
		ioctl(0, KDSETMODE, KD_GRAPHICS);
	else
		ioctl(0, KDSETMODE, KD_TEXT);
	ioctl(0, KDSETMODE, startmode);
	termctl(ECHO, 1);
}

static void
consfinalsig(int sig)
{
	consfinal();
	exit(128+sig);
}

static void
fbproc(void *v)
{
	struct input_event data;
	struct stat ts;
	struct pollfd *pfd;
	int npfd;
	int vt;
	int r;

	if(fstat(0, &ts) < 0)
		panic("fstat current tty: %r");
	vt = MINOR(ts.st_rdev);

	kproc("unblank", unblank, nil);
	kproc("ignore0", ignore0, nil);

	consinit();

	npfd = 1;
	pfd = malloc(sizeof(struct pollfd));
	if(!pfd)
		panic("malloc pollfd: %r");
	pfd->fd = open("/proc/bus/input/devices", O_RDONLY|O_CLOEXEC); // input hotplug
	if (pfd->fd < 0)
		panic("cannot open /proc/bus/input/devices: %r");
	pfd->events = POLLIN;

	qlock(&flushlock);
TOP:
	while(ioctl(0, VT_WAITACTIVE, vt) < 0)
		if (errno != EINTR)
			panic("ioctl VT_WAITACTIVE: %r");
	qunlock(&flushlock);

	qlock(&drawlock);
	flushmemscreen(gscreen->clipr);
	qunlock(&drawlock);

	eventattach(&pfd, &npfd);

	for (;;) {
		if(poll(pfd, npfd, -1) < 0)
			oserror();
		if(atomic_load(&switchaway)) {
			if(altdown){	// Kalt used to switch vt
				kbdsc(0x1d);	// Send Kctl to break the compose sequence
				kbdsc(0x1d|0x80);
				altdown = 0;
			}
			eventdetach(pfd + 1, npfd - 1);
			npfd = 1;
			qlock(&flushlock);
			if(ioctl(0, VT_RELDISP, 1) < 0)
				panic("ioctl VT_RELDISP: %r");
			atomic_store(&switchaway, 0);
			goto TOP;
		}
		if (pfd->revents & POLLIN) {
			eventdetach(pfd + 1, npfd - 1);
			npfd = 1;
			qlock(&flushlock);
			goto TOP;
		}
		for(r = 1; r < npfd; r++)
			if (pfd[r].revents & POLLIN) {
				if (read(pfd[r].fd, &data, sizeof(data)) != sizeof(data))
					panic("eventfd read: %r");
				onevent(&data);
			}
	}

	consfinal();
	eventdetach(pfd, npfd);
	free(pfd);
}

void
screensize(Rectangle r, ulong chan)
{
	gscreen = backbuf;
	gscreen->clipr = ZR;
}

void
screeninit(void)
{
	memimageinit();

	if(fbattach(0) == nil) {
		panic("cannot open framebuffer: %r");
	}

	screensize(screenr, chan);
	if (gscreen == nil)
		panic("screensize failed");

	gscreen->clipr = screenr;
	kproc("fbflush", fbflush, nil);
	kproc("fbdev", fbproc, nil);

	terminit();

	qlock(&drawlock);
	flushmemscreen(gscreen->clipr);
	qunlock(&drawlock);
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
	return;
}

char*
clipread(void)
{
	if(snarfbuf)
		return strdup(snarfbuf);
	return nil;
}

int
clipwrite(char *buf)
{
	if(snarfbuf)
		free(snarfbuf);
	snarfbuf = strdup(buf);
	return 0;
}

void
guimain(void)
{
	cpubody();
}

static void
termctl(uint32_t o, int or)
{
	struct termios t;

	tcgetattr(0, &t);
	if (or)
		t.c_lflag |= o;
	else
		t.c_lflag &= o;
	tcsetattr(0, TCSANOW, &t);
}

static void
ttyswitch(int sig)
{
	atomic_store(&switchaway, 1);
}

int
onevent(struct input_event *data)
{
	Rectangle old, new;
	ulong msec;
	static int buttons;
	static Point coord;
	static char touched;
	static Point startmousept;
	static Point startpt;
	static int mod4down;
	int key;

	msec = ticks();

	old.min = mousexy;
	old.max = addpt(old.min, Pt(16, 16));

	buttons &= ~0x18;

	switch(data->type) {
	case 3:
		switch(data->code) {
		case 0:
			coord.x = data->value;
			break;
		case 1:
			coord.y = data->value;
			break;
		case 0x18:
		case 0x1c:
			if (data->value == 0)
				touched = 0;
			else if (data->value > 24) {
				touched = 1;
				startmousept = coord;
				startpt = mousexy;
			}
			break;
		default:
			return -1;
		}
		if (touched)
			mousexy = addpt(startpt, divpt(subpt(coord, startmousept), 4));
		break;
	case 2:
		switch(data->code) {
		case 0:
			mousexy.x += data->value;
			break;
		case 1:
			mousexy.y += data->value;
			break;
		case 8:
			buttons |= data->value == 1? 8: 16;
			break;
		default:
			return -1;
		}
		break;
	case 1:
		switch(data->code) {
		case 0x110:
			if (data->value == 1)
				buttons |= 1;
			else
				buttons &= ~1;
			break;
		case 0x111:
			if (data->value == 1)
				buttons |= 4;
			else
				buttons &= ~4;
			break;
		case 0x112:
			if (data->value == 1)
				buttons |= 2;
			else
				buttons &= ~2;
			break;
		default:
			/* Convert linux keycode to 9front scancode
			 * linux: input-event-codes.h
			 * 9front: kbdfs.c
			 */
			if (data->code > 0 && data->code <= 0x58)
				key = data->code;
			else switch(data->code) {
			case 96:
				key = 0x1c; // '\n'
				break;
			case 97:
				key = 0x1d; // Kctl
				break;
			case 98:
				key = 0x35; // '/'
				break;
			case 100:
				key = 0x38; // Kalt
				break;
			case 102:
				key = 0xe047; // Khome
				break;
			case 103:
				key = 0xe048; // Kup
				break;
			case 104:
				key = 0xe049; // Kpgup
				break;
			case 105:
				key = 0xe04b; // Kleft
				break;
			case 106:
				key = 0xe04d; // Kright
				break;
			case 107:
				key = 0xe04f; // Kend
				break;
			case 108:
				key = 0xe050; // Kdown
				break;
			case 109:
				key = 0xe051; // Kpgdown
				break;
			case 110:
				key = 0xe052; // Kins
				break;
			case 111:
				key = 0xe053; // Kdel
				break;
			case 125:
			case 126:
				key = 0xe05b; // Kmod4
				break;
			default:
				fprint(2, "DEBUG: got key data->code=%d\n", data->code);
				break;
			}
			if(!data->value)
				key |= 0x80;
			if(key == 0x38)
				altdown = 1;
			else if(key == (0x38|0x80))
				altdown = 0;
			else if(key == 0xe05b)
				mod4down = 1;
			else if(key == (0xe05b|0x80))
				mod4down = 0;
			else if(key >= 0x3b && key <= 0x44 && (altdown | mod4down))
				return 0;	// Ignore alt/mod4-F1~F10 for console switching
			kbdsc(key);
			return 0;
		}
		break;
	default:
		return -1;
	}

	if (mousexy.x < screenimage->r.min.x)
		mousexy.x = screenimage->r.min.x;
	if (mousexy.y < screenimage->r.min.y)
		mousexy.y = screenimage->r.min.y;
	if (mousexy.x > screenimage->r.max.x)
		mousexy.x = screenimage->r.max.x;
	if (mousexy.y > screenimage->r.max.y)
		mousexy.y = screenimage->r.max.y;
	
	new.min = mousexy;
	new.max = addpt(new.min, Pt(16, 16)); // size of cursor bitmap

	combinerect(&new, old);
	new.min = subpt(new.min, Pt(16, 16)); // to encompass any cursor->offset

	qlock(&drawlock);
	flushmemscreen(new);
	qunlock(&drawlock);

	absmousetrack(mousexy.x, mousexy.y, buttons, msec);

	return 0;
}

void
mouseset(Point p)
{
	qlock(&drawlock);
	mousexy = p;
	flushmemscreen(screenr);
	qunlock(&drawlock);
}

void
setcursor(void)
{
	qlock(&drawlock);
	flushmemscreen(screenr);
	qunlock(&drawlock);
}

void
titlewrite(char* buf)
{
}

