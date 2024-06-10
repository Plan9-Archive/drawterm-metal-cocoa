#define _GNU_SOURCE

#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include <cursor.h>

#undef up

#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-protocol.h"

#include "screen.h"
#include "wl-inc.h"

static int
wlcreateshm(off_t size)
{
	char name[] = "/drawterm--XXXXXX";
	char *dir, *path;
	int fd;

	if((dir = getenv("XDG_RUNTIME_DIR")) == nil)
		panic("XDG_RUNTIME_DIR not set");

	path = malloc(strlen(dir) + sizeof(name) + 1);
	strcpy(path, dir);
	strcat(path, name);

	if((fd = mkostemp(path, O_CLOEXEC)) >= 0)
		unlink(path);
	free(path);

	return fd;
}

void
wlallocpool(Wlwin *wl)
{
	int screensize, cursorsize;
	int depth;
	int fd;

	if(wl->pool != nil)
		wl_shm_pool_destroy(wl->pool);

	depth = 4;
	screensize = wl->monx * wl->mony * depth;
	cursorsize = 32 * 32 * depth;

	fd = wlcreateshm(screensize+cursorsize);
	if(fd < 0)
		panic("could not mk_shm_fd");
	if(ftruncate(fd, screensize+cursorsize) < 0)
		panic("could not ftruncate");

	wl->shm_data = mmap(nil, screensize+cursorsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(wl->shm_data == MAP_FAILED)
		panic("could not mmap shm_data");

	wl->pool = wl_shm_create_pool(wl->shm, fd, screensize+cursorsize);
	wl->poolsize = screensize+cursorsize;
	close(fd);
}

void
wlallocbuffer(Wlwin *wl)
{
	int depth;
	int size;

	depth = 4;
	size = wl->dx * wl->dy * depth;
	if(wl->pool == nil || size+(16*16*depth) > wl->poolsize)
		wlallocpool(wl);

	assert(size+(16*16*depth) <= wl->poolsize);

	if(wl->screenbuffer != nil)
		wl_buffer_destroy(wl->screenbuffer);
	if(wl->cursorbuffer != nil)
		wl_buffer_destroy(wl->cursorbuffer);

	wl->screenbuffer = wl_shm_pool_create_buffer(wl->pool, 0, wl->dx, wl->dy, wl->dx*4, WL_SHM_FORMAT_XRGB8888);
	wl->cursorbuffer = wl_shm_pool_create_buffer(wl->pool, size, 32, 32, 32*4, WL_SHM_FORMAT_ARGB8888);
}

enum {
	White = 0xFFFFFFFF,
	Black = 0xFF000000,
	Green = 0xFF00FF00,
	Transparent = 0x00000000,
};

void
wldrawcursor(Wlwin *wl, Cursorinfo *c)
{
	int i, j;
	int pos, mask;
	u32int *buf;
	uint16_t clr[16], set[16];

	buf = wl->shm_data+(wl->dx*wl->dy*4);
	for(i=0,j=0; i < 16; i++,j+=2){
		clr[i] = c->clr[j]<<8 | c->clr[j+1];
		set[i] = c->set[j]<<8 | c->set[j+1];
	}
	for(i=0; i < 32; i++){
		for(j = 0; j < 32; j++){
			pos = i*32 + j;
			mask = (1<<16) >> j;

			buf[pos] = Transparent;
			if(i < 16 && clr[i] & mask)
				buf[pos] = White;
			if(i < 16 && set[i] & mask)
				buf[pos] = Black;
		}
	}
	if(wl->cursorsurface == nil)
		wl->cursorsurface = wl_compositor_create_surface(wl->compositor);
	wl_surface_attach(wl->cursorsurface, wl->cursorbuffer, 0, 0);
	wl_surface_damage(wl->cursorsurface, 0, 0, 32, 32);
	wl_surface_commit(wl->cursorsurface);
	wl_pointer_set_cursor(wl->pointer, wl->pointerserial, wl->cursorsurface, -c->offset.x, -c->offset.y);
}
