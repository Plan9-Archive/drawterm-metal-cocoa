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

#include <linux/fb.h>
#include <linux/input.h>

Rectangle	update_rect;
uchar*		fbp;
Memimage*	screenimage;
Memimage*	backbuf;
Rectangle	screenr;
char*		snarfbuf;
struct fb_fix_screeninfo finfo;
struct fb_var_screeninfo vinfo;
int		*eventfds = NULL;
int		neventfds;
Point		mousexy;
char		shift_state;
int		ttyfd;
char*		tty;
char		hidden;
int		devicesfd;
ulong		chan;
int		depth;

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <limits.h>

#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/keyboard.h>
#include <signal.h>

#include <termios.h>

#define ulong p9_ulong

Memimage *gscreen;
char *snarfbuf = nil;

int onevent(struct input_event*);
void termctl(uint32_t o, int or);
void ctrlc(int sig);

void
_fbput(Memimage *m, Rectangle r) {
	int y;

	for (y = r.min.y; y < r.max.y; y++){
		long loc = y * finfo.line_length + r.min.x * depth;
		void *ptr = m->data->bdata + y * m->width * 4 + r.min.x * depth;

		memcpy(fbp + loc, ptr, Dx(r) * depth);
	}
}

Memimage*
fbattach(int fbdevidx)
{
	Rectangle r;
	char devname[64];
	size_t size;
	int fd;

	/*
	 * Connect to /dev/fb0
	 */
	snprintf(devname, sizeof(devname) - 1, "/dev/fb%d", fbdevidx);
	if ((fd = open(devname, O_RDWR)) < 0)
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

	size = vinfo.yres_virtual * finfo.line_length;
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

int
eventattach()
{
	char eventfile[PATH_MAX] = "";
	char line[PATH_MAX];
	FILE *devices;
	char *ptr;

	neventfds = 0;
	devices = fopen("/proc/bus/input/devices", "r");
	if (devices == NULL)
		return -1;
	while (fgets(line, sizeof(line)-1, devices) != NULL)
		if (line[0] == 'H') {
			ptr = strstr(line, "event");
			if (ptr == NULL)
				continue;
			ptr[strcspn(ptr, " \r\n")] = '\0';
			snprintf(eventfile, sizeof(eventfile)-1, "/dev/input/%s", ptr);
			neventfds++;
			eventfds = realloc(eventfds, neventfds * sizeof(int));
			eventfds[neventfds-1] = open(eventfile, O_RDONLY);
			if (eventfds[neventfds-1] < 0)
				neventfds--;
		}
	fclose(devices);

	if (neventfds == 0)
		return -1;
	return 1;
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
		ms = ticks();

		qlock(&drawlock);
		r = update_rect;
		update_rect = Rect(0,0,0,0);
		if (Dx(r) == 0 || Dy(r) == 0 || hidden != 0){
			qunlock(&drawlock);
			goto wait;
		}
		memimagedraw(screenimage, r, backbuf, r.min, nil, r.min, S);
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

wait:
		ms = ms + del - ticks();
		if(ms > 0 && ms < del)
			osmsleep(ms);
	}
}

static void
fbproc(void *v)
{
	struct input_event data;
	char buf[32];
	struct pollfd *pfd;
	int r;
	int ioctlarg;

	pfd = calloc(3, sizeof(struct pollfd));
	pfd[0].fd = ttyfd; // for virtual console switches
	pfd[0].events = POLLPRI;
	pfd[1].fd = 0; // stdin goes to nowhere
	pfd[1].events = POLLIN;
	pfd[2].fd = open("/proc/bus/input/devices", O_RDONLY); // input hotplug
	if (pfd[2].fd < 0)
		panic("cannot open /proc/bus/input/devices: %r");
	pfd[2].events = POLLIN;

TOP:
	while(read(pfd[2].fd, buf, 31) > 0);

	pfd = realloc(pfd, sizeof(struct pollfd) * (neventfds + 3));
	for (r = 0; r < neventfds; r++) {
		pfd[r+3].fd = eventfds[r];
		pfd[r+3].events = POLLIN;
	}

	for(;;) {
		shift_state = 6;
		if (ioctl(0, TIOCLINUX, &shift_state) < 0)
			panic("ioctl TIOCLINUX 6: %r");

		r = poll(pfd, 3+neventfds, -1);
		if (r < 0)
			oserror();
		if (pfd[0].revents & POLLPRI) {
			if ((r = read(ttyfd, buf, 31)) <= 0)
				panic("ttyfd read: %r");
			buf[r] = '\0';
			if (strcmp(buf, tty) == 0) {
				hidden = 0;
				printf("\e[?25l");
				fflush(stdout);
				qlock(&drawlock);
				flushmemscreen(gscreen->clipr);
				qunlock(&drawlock);
			}
			else
				hidden = 1;
			close(ttyfd);
			ttyfd = open("/sys/class/tty/tty0/active", O_RDONLY);
			if (ttyfd < 0)
				panic("cannot open tty active fd: %r");
			pfd[0].fd = ttyfd;
			read(ttyfd, buf, 0);
		}
		if (pfd[1].revents & POLLIN)
			read(pfd[1].fd, buf, 31);
		if (pfd[2].revents & POLLIN) {
			for (r = 0; r < neventfds; r++)
				close(eventfds[r]);
			if(eventattach() < 0) {
				panic("cannot open event files: %r");
			}
			goto TOP;
		}
		for (r = 0; r < neventfds; r++)
			if (pfd[r+3].revents & POLLIN) {
				if (read(pfd[r+3].fd, &data, sizeof(data)) != sizeof(data))
					panic("eventfd read: %r");
				if (onevent(&data) == 0) {
					ioctlarg = 15;
					if (ioctl(0, TIOCLINUX, &ioctlarg) != 0) {
						ioctlarg = 4;
						ioctl(0, TIOCLINUX, &ioctlarg);
						qlock(&drawlock);
						flushmemscreen(gscreen->clipr);
						qunlock(&drawlock);
					} else {
						write(1, "\033[9;30]", 7);
					}
				}
			}
	}

	printf("\e[?25h");
	fflush(stdout);
	termctl(ECHO, 1);
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
	int r;
	char buf[1];

	// set up terminal
	printf("\e[?25l");
	fflush(stdout);
	termctl(~(ICANON|ECHO), 0);
	signal(SIGINT, ctrlc);

	memimageinit();

	// tty switching
	ttyfd = open("/sys/class/tty/tty0/active", O_RDONLY);
	if (ttyfd >= 0) {
		tty = malloc(32);
		r = read(ttyfd, tty, 31);
		if (r >= 0)
			tty[r] = '\0';
		else
			tty[0] = '\0';
		close(ttyfd);
		ttyfd = open("/sys/class/tty/tty0/active", O_RDONLY);
	}
	if (ttyfd < 0)
		panic("cannot open tty active fd: %r");
	read(ttyfd, buf, 0);
	hidden = 0;

	if(fbattach(0) == nil) {
		panic("cannot open framebuffer: %r");
	}

	if(eventattach() < 0) {
		panic("cannot open event files: %r");
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

void
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

void
ctrlc(int sig) {
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
	int key;
	static ulong lastmsec = 0;

	if (hidden != 0)
		return -1;

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
				buttons |= shift_state & (1 << KG_SHIFT)? 2: 4;
			else
				buttons &= ~(shift_state & (1 << KG_SHIFT)? 2: 4);
			break;
		case 0x112:
			if (data->value == 1)
				buttons |= 2;
			else
				buttons &= ~2;
			break;
		default:
			if (hidden)
				return 0;
			/* Convert linux keycode to 9front scancode
			 * linux: input_event_codes.h
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
			}
			if(!data->value)
				key |= 0x80;
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

	if ((msec - lastmsec) < 10)
		if (data->type != 1)
			return 0;

	lastmsec = msec;

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

