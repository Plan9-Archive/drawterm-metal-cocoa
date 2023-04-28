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

static Wlwin *gwin;

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
	wl->monx = wl->dx;
	wl->mony = wl->dy;
	return wl;
}

void
wlclose(Wlwin *wl)
{
	wl->runing = 0;
	exits(nil);
}

void
wltogglemaximize(Wlwin *wl)
{
	if(wl->maximized)
		xdg_toplevel_unset_maximized(wl->xdg_toplevel);
	else
		xdg_toplevel_set_maximized(wl->xdg_toplevel);
}

void
wlminimize(Wlwin *wl)
{
	xdg_toplevel_set_minimized(wl->xdg_toplevel);
}

void
wlmove(Wlwin *wl, uint32_t serial)
{
	xdg_toplevel_move(wl->xdg_toplevel, wl->seat, serial);
}

void
wlmenu(Wlwin *wl, uint32_t serial)
{
	xdg_toplevel_show_window_menu(wl->xdg_toplevel, wl->seat, serial, wl->mouse.xy.x, wl->mouse.xy.y);
}

static void
wlupdatecsdrects(Wlwin *wl)
{
	Point offset;
	Rectangle button;

	if(!wl->client_side_deco) {
		memset(&wl->csd_rects, 0, sizeof wl->csd_rects);
		return;
	}

	wl->csd_rects.bar = Rect(0, 0, wl->dx, csd_bar_height);

	offset = Pt(csd_button_width + 4, 0);
	button = Rect(0, 4, csd_button_width, csd_button_width + 4);
	button = rectsubpt(button, offset);

	wl->csd_rects.button_close = button = rectaddpt(button, Pt(wl->dx, 0));
	wl->csd_rects.button_maximize = button = rectsubpt(button, offset);
	wl->csd_rects.button_minimize = rectsubpt(button, offset);
}

static void
wlfillrect(Wlwin *wl, Rectangle rect, uint32_t color)
{
	Point p;
	uint32_t *data = wl->shm_data;

	for(p.y = rect.min.y; p.y < rect.max.y; p.y++)
		for(p.x = rect.min.x; p.x < rect.max.x; p.x++)
			data[p.y * wl->dx + p.x] = color;
}

static void
wldrawcsd(Wlwin *wl)
{
	if(!wl->client_side_deco)
		return;
	wlfillrect(wl, wl->csd_rects.bar, 0xAAAAAA);
	wlfillrect(wl, wl->csd_rects.button_close, DRed >> 8);
	wlfillrect(wl, wl->csd_rects.button_maximize, DGreen >> 8);
	wlfillrect(wl, wl->csd_rects.button_minimize, DYellow >> 8);
}

void
wlflush(Wlwin *wl)
{
	Point p;

	wl_surface_attach(wl->surface, wl->screenbuffer, 0, 0);
	if(wl->dirty){
		p.x = wl->r.min.x;
		for(p.y = wl->r.min.y; p.y < wl->r.max.y; p.y++)
			memcpy(wl->shm_data+(p.y*wl->dx+p.x)*4, byteaddr(gscreen, p), Dx(wl->r)*4);
		wl_surface_damage(wl->surface, p.x, wl->r.min.y, Dx(wl->r), Dy(wl->r));
		wl->dirty = 0;
	}
	wl_surface_commit(wl->surface);
}

void  _screenresize(Rectangle);

void
wlresize(Wlwin *wl, int x, int y)
{
	Rectangle r;

	wl->dx = x;
	wl->dy = y;
	wlupdatecsdrects(wl);

	qlock(&drawlock);
	wlallocbuffer(wl);
	r = Rect(0, wl->csd_rects.bar.max.y, wl->dx, wl->dy);
	gscreen = allocmemimage(r, XRGB32);
	gscreen->clipr = ZR;
	qunlock(&drawlock);

	screenresize(r);

	qlock(&drawlock);
	wl->dirty = 1;
	wl->r = r;
	wlflush(wl);
	wldrawcsd(wl);
	qunlock(&drawlock);
}

void
dispatchproc(void *a)
{
	Wlwin *wl;
	wl = a;
	while(wl->runing)
		wl_display_dispatch(wl->display);
}

static Wlwin*
wlattach(char *label)
{
	Rectangle r;
	Wlwin *wl;

	wl = newwlwin();
	gwin = wl;
	wl->display = wl_display_connect(nil);
	if(wl->display == nil)
		sysfatal("could not connect to display");

	memimageinit();
	wlsetcb(wl);
	wlupdatecsdrects(wl);
	wlflush(wl);
	wlsettitle(wl, label);

	r = Rect(0, wl->csd_rects.bar.max.y, wl->dx, wl->dy);
	gscreen = allocmemimage(r, XRGB32);
	gscreen->clipr = r;
	gscreen->r = r;
	rectclip(&(gscreen->clipr), gscreen->r);

	wl->runing = 1;
	kproc("wldispatch", dispatchproc, wl);
	terminit();
	qlock(&drawlock);
	wlflush(wl);
	wldrawcsd(wl);
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
	gwin->dirty = 1;
	gwin->r = r;
	wlflush(gwin);
}

void
screensize(Rectangle r, ulong chan)
{
	flushmemscreen(r);
}

void
setcursor(void)
{
	qlock(&drawlock);
	wldrawcursor(gwin, &cursor);
	qunlock(&drawlock);
}

void
mouseset(Point p)
{
	wlsetmouse(gwin, p);
}

char*
clipread(void)
{
	return wlgetsnarf(gwin);
}

int
clipwrite(char *data)
{
	wlsetsnarf(gwin, data);
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
