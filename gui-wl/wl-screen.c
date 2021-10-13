#define _POSIX_C_SOURCE 200809L
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-protocol.h"

#include "u.h"
#include "lib.h"
#include "kern/dat.h"
#include "kern/fns.h"
#include "error.h"
#include "user.h"
#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include <cursor.h>
#include "screen.h"
#include "wl-inc.h"

#undef close

static Wlwin *snarfwin;

static int clientruning;

Memimage *gscreen;

static Wlwin*
newwlwin(void)
{
	Wlwin *wl;

	wl = mallocz(sizeof *wl, 1);
	if(wl == nil)
		sysfatal("malloc Wlwin");
	wl->dx = 1024;
	wl->dy = 1024;
	wl->monx = 1920;
	wl->mony = 1080;
	return wl;
}

void
wlflush(Wlwin *wl)
{
	if(wl->dirty == 1)
		memcpy(wl->shm_data, gscreen->data->bdata, wl->dx*wl->dy*4);

	wl_surface_attach(wl->surface, wl->screenbuffer, 0, 0);
	wl_surface_damage(wl->surface, 0, 0, wl->dx, wl->dy);
	wl_surface_commit(wl->surface);
	wl->dirty = 0;
}

void
wlresize(Wlwin *wl, int x, int y)
{
	Rectangle r;

	wl->dx = x;
	wl->dy = y;

	qlock(&drawlock);
	wlallocbuffer(wl);
	r = Rect(0, 0, wl->dx, wl->dy);
	gscreen = allocmemimage(r, XRGB32);
	gscreen->clipr = ZR;
	qunlock(&drawlock);

	screenresize(r);

	qlock(&drawlock);
	wl->dirty = 1;
	wlflush(wl);
	qunlock(&drawlock);
}

void
dispatchproc(void *a)
{
	Wlwin *wl;
	wl = a;
	for(;wl->runing == 1;){
		wl_display_dispatch(wl->display);
	}
}

static Wlwin*
wlattach(char *label)
{
	Rectangle r;
	Wlwin *wl;

	wl = newwlwin();
	snarfwin = wl;
	wl->display = wl_display_connect(NULL);
	if(wl->display == nil)
		sysfatal("could not connect to display");

	memimageinit();
	wlsetcb(wl);
	wlflush(wl);
	wlsettitle(wl, label);

	r = Rect(0, 0, wl->dx, wl->dy);
	gscreen = allocmemimage(r, XRGB32);
	gscreen->clipr = r;
	gscreen->r = r;
	rectclip(&(gscreen->clipr), gscreen->r);

	wl->runing = 1;
	kproc("wldispatch", dispatchproc, wl);
	terminit();
	qlock(&drawlock);
	wlflush(wl);
	qunlock(&drawlock);
	return wl;
}

void
screeninit(void)
{
	wlattach("drawterm");
}

void
guimain(void)
{
	cpubody();
}

Memdata*
attachscreen(Rectangle *r, ulong *chan, int *depth, int *width, int *softscreen)
{
	Wlwin *wl;

	wl = snarfwin;
	*r = gscreen->clipr;
	*chan = gscreen->chan;
	*depth = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;

	gscreen->data->ref++;
	return gscreen->data;
}

void
flushmemscreen(Rectangle r)
{
	Wlwin *wl;

	wl = snarfwin;
	wl->dirty = 1;
	wlflush(wl);
}

void
screensize(Rectangle r, ulong chan)
{
	snarfwin->dirty = 1;
}

void
setcursor(void)
{
	qlock(&drawlock);
	wldrawcursor(snarfwin, &cursor);
	qunlock(&drawlock);
}

void
mouseset(Point p)
{
}

char*
clipread(void)
{
	return wlgetsnarf(snarfwin);
}

int
clipwrite(char *data)
{
	wlsetsnarf(snarfwin, data);
	return strlen(data);
}

void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
}

void
setcolor(ulong index, ulong red, ulong green, ulong blue)
{
}
