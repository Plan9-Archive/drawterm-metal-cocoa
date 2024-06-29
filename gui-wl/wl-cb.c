#define _GNU_SOURCE

#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>

#undef up

#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "xdg-primary-selection-protocol.h"
#include "wlr-virtual-pointer.h"

#include "screen.h"
#include "wl-inc.h"


static void
xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	Wlwin *wl;

	wl = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	wl_surface_commit(wl->surface);
}

const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	wlclose(data);
}

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states)
{
	Wlwin *wl;
	enum xdg_toplevel_state state;
	int i;

	wl = data;
	if(width == 0 || height == 0 || (width == wl->dx && height == wl->dy))
		return;
	wlresize(wl, width, height);

	wl->maximized = 0;
	for(i = 0; i < states->size; i++){
		state = ((enum xdg_toplevel_state *)states->data)[i];
		if(state == XDG_TOPLEVEL_STATE_MAXIMIZED)
			wl->maximized = 1;
	}
}

const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	Wlwin *wl;

	wl = data;
	wl_callback_destroy(cb);
	cb = wl_surface_frame(wl->surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, wl);
	qlock(&drawlock);
	wlflush(wl);
	qunlock(&drawlock);
}

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
	static struct xkb_keymap *keymap = nil;
	char *keymap_string;
	Wlwin *wl;

	wl = data;
	keymap_string = mmap(nil, size, PROT_READ, MAP_SHARED, fd, 0);
	xkb_keymap_unref(keymap);
	keymap = xkb_keymap_new_from_string(wl->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(keymap_string, size);
	close(fd);
	xkb_state_unref(wl->xkb_state);
	wl->xkb_state = xkb_state_new(keymap);
}

static void
keyboard_enter (void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
	Wlwin *wl;

	wl = data;
	qlock(&wl->clip.lk);
	wl->clip.serial = serial;
	qunlock(&wl->clip.lk);
}

static struct {
	Rendez z;
	QLock lk;
	int active;
	long keytime;
	int32_t key;
	int32_t rate;
	int32_t delay;
} repeatstate;

static int
isactive(void *arg)
{
	return repeatstate.active;
}

void
repeatproc(void *_dummy)
{
	int ms;
	long keytime;

	USED(_dummy);
	for(;;){
		ksleep(&repeatstate.z, isactive, 0);
		qlock(&repeatstate.lk);
		keytime = repeatstate.keytime;
		qunlock(&repeatstate.lk);
		osmsleep(repeatstate.delay);

repeat:
		qlock(&repeatstate.lk);
		if(repeatstate.active == 0 || keytime != repeatstate.keytime){
			qunlock(&repeatstate.lk);
			continue;
		}
		ms = 1000/repeatstate.rate;
		kbdkey(repeatstate.key, 0);
		kbdkey(repeatstate.key, 1);
		qunlock(&repeatstate.lk);
		osmsleep(ms);
		goto repeat;
	}
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay)
{
	qlock(&repeatstate.lk);
	repeatstate.rate = rate;
	repeatstate.delay = delay;
	qunlock(&repeatstate.lk);
}

static void
keyboard_leave (void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
	Wlwin *wl;

	wl = data;
	kbdkey(Kshift, 0);
	kbdkey(Kmod4, 0);
	kbdkey(Kctl, 0);
	kbdkey(Kalt, 0);
	if(wl->alt != Aunpress){
		kbdkey(Kalt, 1);
		kbdkey(Kalt, 0);
		wl->alt = Aunpress;
	}
	qlock(&repeatstate.lk);
	repeatstate.active = 0;
	repeatstate.key = 0;
	qunlock(&repeatstate.lk);
}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	Wlwin *wl;
	uint32_t utf32;
	int repeat;

	wl = data;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(wl->xkb_state, key+8);
	switch(keysym) {
	case XKB_KEY_Return:
		utf32 = '\n';
		break;
	case XKB_KEY_Tab:
		utf32 = '\t';
		break;
	case XKB_KEY_Up:
		utf32 = Kup;
		break;
	case XKB_KEY_Down:
		utf32 = Kdown;
		break;
	case XKB_KEY_Left:
		utf32 = Kleft;
		break;
	case XKB_KEY_Right:
		utf32 = Kright;
		break;
	case XKB_KEY_Page_Up:
		utf32 = Kpgup;
		break;
	case XKB_KEY_Page_Down:
		utf32 = Kpgdown;
		break;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		utf32 = Kctl;
		break;
	case XKB_KEY_Alt_R:
		utf32 = Kaltgr;
		break;
	case XKB_KEY_Alt_L:
		utf32 = Kalt;
		break;
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		utf32 = Kshift;
		break;
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
		utf32 = Kmod4;
		break;
	case XKB_KEY_End:
		utf32 = Kend;
		break;
	case XKB_KEY_Begin:
		utf32 = Khome;
		break;
	case XKB_KEY_Insert:
		utf32 = Kins;
		break;
	case XKB_KEY_Scroll_Lock:
		utf32 = Kscroll;
		break;
	case XKB_KEY_F1:
	case XKB_KEY_F2:
	case XKB_KEY_F3:
	case XKB_KEY_F4:
	case XKB_KEY_F5:
	case XKB_KEY_F6:
	case XKB_KEY_F7:
	case XKB_KEY_F8:
	case XKB_KEY_F9:
	case XKB_KEY_F10:
	case XKB_KEY_F11:
	case XKB_KEY_F12:
		utf32 = KF|(keysym - XKB_KEY_F1 + 1);
		break;
	case XKB_KEY_XF86AudioPrev:
		utf32 = Ksbwd;
		break;
	case XKB_KEY_XF86AudioNext:
		utf32 = Ksfwd;
		break;
	case XKB_KEY_XF86AudioPlay:
		utf32 = Kpause;
		break;
	case XKB_KEY_XF86AudioLowerVolume:
		utf32 = Kvoldn;
		break;
	case XKB_KEY_XF86AudioRaiseVolume:
		utf32 = Kvolup;
		break;
	case XKB_KEY_XF86AudioMute:
		utf32 = Kmute;
		break;

	/* Japanese layout; see /sys/lib/kbmap/jp */
	case XKB_KEY_Muhenkan:
		utf32 = 0x0c; /* ^l */
		break;
	case XKB_KEY_Henkan:
		utf32 = 0x1c; /* ^\ */
		break;
	case XKB_KEY_Hiragana:
		utf32 = 0x0e; /* ^n */
		break;
	case XKB_KEY_Katakana:
		utf32 = 0x0b; /* ^k */
		break;
	case XKB_KEY_Hiragana_Katakana:
		/* board may not maintain kana state */
		if(xkb_state_mod_name_is_active(wl->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
			utf32 = 0x0b;
		else
			utf32 = 0x0e;
		break;
	default:
		utf32 = xkb_keysym_to_utf32(keysym);
		break;
	}
	if(utf32 == 0)
		return;

	if(state == 1){
		if(utf32 == Kalt){
			if(wl->alt == Aunpress)
				wl->alt = Apress;
			else
				wl->alt = Aunpress;
		} else {
			switch(wl->alt){
			case Apress:
			case Aenter1:
				wl->alt++;
				break;
			case Aenter2:
				wl->alt = Aunpress;
			}
		}
	}
	repeat = state && utf32 != Kctl && utf32 != Kshift && utf32 != Kalt && utf32 != Kmod4;
	kbdkey(utf32, state);
	qlock(&repeatstate.lk);
	repeatstate.active = repeat;
	repeatstate.keytime = time;
	repeatstate.key = utf32;
	qunlock(&repeatstate.lk);
	wakeup(&repeatstate.z);
}

static void
keyboard_modifiers (void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
	Wlwin *wl;

	wl = data;
	xkb_state_update_mask(wl->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done,
};

static struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

enum{
	P9Mouse1 = 1,
	P9Mouse2 = 2,
	P9Mouse3 = 4,
};

static int
csd_handle_mouse(Wlwin *wl, uint32_t button, uint32_t serial)
{
	if(ptinrect(wl->mouse.xy, wl->csd_rects.button_close)){
		wlclose(wl);
		return 1;
	}
	if(ptinrect(wl->mouse.xy, wl->csd_rects.button_maximize)){
		wltogglemaximize(wl);
		return 1;
	}
	if(ptinrect(wl->mouse.xy, wl->csd_rects.button_minimize)){
		wlminimize(wl);
		return 1;
	}
	if(ptinrect(wl->mouse.xy, wl->csd_rects.bar)){
		switch (button) {
		case BTN_LEFT: wlmove(wl, serial); break;
		case BTN_RIGHT: wlmenu(wl, serial); break;
		}
		return 1;
	}
	return 0;
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	Wlwin *wl;
	int m;

	wl = data;
	switch(button){
	case BTN_LEFT: m = P9Mouse1; break;
	case BTN_MIDDLE: m = P9Mouse2; break;
	case BTN_RIGHT: m = P9Mouse3; break;
	default: m = 0; break;
	}

	if(state)
		wl->mouse.buttons |= m;
	else
		wl->mouse.buttons &= ~m;

	wl->mouse.msec = time;
	if(state && wl->client_side_deco && csd_handle_mouse(wl, button, serial))
		return;

	absmousetrack(wl->mouse.xy.x, wl->mouse.xy.y, wl->mouse.buttons, wl->mouse.msec);
}

static void
pointer_handle_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Wlwin *wl;

	wl = data;
	wl->mouse.xy.x = surface_x / 256;
	wl->mouse.xy.y = surface_y / 256;
	wl->mouse.msec = time;
	absmousetrack(wl->mouse.xy.x, wl->mouse.xy.y, wl->mouse.buttons, wl->mouse.msec);
}

static void
pointer_handle_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	Wlwin *wl;

	wl = data;
	wl->pointerserial = serial;
	pointer_handle_motion(data, wl_pointer, wl->mouse.msec, surface_x, surface_y);
	setcursor();
}

static void
pointer_handle_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	Wlwin *wl;
	int buttons;

	if(axis == 1)
		return; /* Horizontal scroll */
	wl = data;
	wl->mouse.msec = time;
	/* p9 expects a scroll event to work like a button, a set and a release */
	buttons = wl->mouse.buttons & ~24;
	absmousetrack(wl->mouse.xy.x, wl->mouse.xy.y, buttons | (value > 0 ? 16 : 8), wl->mouse.msec);
	absmousetrack(wl->mouse.xy.x, wl->mouse.xy.y, buttons, wl->mouse.msec);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_handle_enter,
	.leave = pointer_handle_leave,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = pointer_handle_axis,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	Wlwin *wl;
	int pointer, keyboard;

	wl = data;
	pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if(pointer && wl->pointer == nil){
		wl->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
	}else if(!pointer && wl->pointer != nil){
		wl_pointer_release(wl->pointer);
		wl->pointer = nil;
	}

	keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
	if(keyboard && wl->keyboard == nil){
		wl->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(wl->keyboard, &keyboard_listener, wl);
	}else if(!keyboard && wl->keyboard != nil){
		wl_keyboard_release(wl->keyboard);
		wl->keyboard = nil;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

static void
data_source_handle_send(void *data, struct wl_data_source *source, const char *mime_type, int fd)
{
	ulong n;
	ulong pos;
	ulong len;
	Wlwin *wl;

	if(strcmp(mime_type, "text/plain;charset=utf-8") != 0)
		return;

	wl = data;
	qlock(&wl->clip.lk);
	len = strlen(wl->clip.content);
	for(pos = 0; (n = write(fd, wl->clip.content+pos, len-pos)) > 0 && pos < len; pos += n)
		;
	close(fd);
	qunlock(&wl->clip.lk);
}

static void
data_source_handle_cancelled(void *data, struct wl_data_source *source)
{
	wl_data_source_destroy(source);
}

static void
data_source_handle_target(void *data, struct wl_data_source *source, const char *mime_type)
{

}

static const struct wl_data_source_listener data_source_listener = {
	.target = data_source_handle_target,
	.send = data_source_handle_send,
	.cancelled = data_source_handle_cancelled,
};

static void
primsel_source_handle_send(void *data, struct zwp_primary_selection_source_v1 *source, const char *mime_type, int fd)
{
	ulong n;
	ulong pos;
	ulong len;
	Wlwin *wl;

	if(strcmp(mime_type, "text/plain;charset=utf-8") != 0)
		return;

	wl = data;
	qlock(&wl->clip.lk);
	len = strlen(wl->clip.content);
	for(pos = 0; (n = write(fd, wl->clip.content+pos, len-pos)) > 0 && pos < len; pos += n)
		;
	close(fd);
	qunlock(&wl->clip.lk);
}

static void
primsel_source_handle_cancelled(void *data, struct zwp_primary_selection_source_v1 *source)
{
	zwp_primary_selection_source_v1_destroy(source);
}

static const struct zwp_primary_selection_source_v1_listener primsel_source_listener = {
	.send = primsel_source_handle_send,
	.cancelled = primsel_source_handle_cancelled,
};

static void
data_device_drop_enter(void* data, struct wl_data_device* wl_data_device, uint serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* id)
{

}

static void
data_device_drop_motion(void* data, struct wl_data_device* wl_data_device, uint time, wl_fixed_t x, wl_fixed_t y)
{

}

static void
data_device_drop_leave(void* data, struct wl_data_device* wl_data_device)
{

}

static void
data_device_handle_data_offer(void *data, struct wl_data_device *data_device, struct wl_data_offer *offer)
{
}

static void
data_device_drop_drop(void* data, struct wl_data_device* wl_data_device)
{
}

static void
data_device_handle_selection(void *data, struct wl_data_device *data_device, struct wl_data_offer *offer)
{
	Wlwin *wl;
	ulong n;
	ulong size;
	ulong pos;
	int fds[2];

	// An application has set the clipboard contents
	if(offer == nil)
		return;

	wl = data;
	pipe2(fds, O_CLOEXEC);
	wl_data_offer_receive(offer, "text/plain;charset=utf-8", fds[1]);
	close(fds[1]);

	wl_display_roundtrip(wl->display);

	qlock(&wl->clip.lk);
	size = 8192;
	wl->clip.content = realloc(wl->clip.content, size+1);
	memset(wl->clip.content, 0, size+1);
	for(pos = 0; (n = read(fds[0], wl->clip.content+pos, size-pos)) > 0;){
		pos += n;
		if(pos >= size){
			size *= 2;
			wl->clip.content = realloc(wl->clip.content, size+1);
			memset(wl->clip.content+pos, 0, (size-pos)+1);
		}
	}
	close(fds[0]);
	qunlock(&wl->clip.lk);
	wl_data_offer_destroy(offer);
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_handle_data_offer,
	.selection = data_device_handle_selection,
	.leave = data_device_drop_leave,
	.motion = data_device_drop_motion,
	.enter = data_device_drop_enter,
	.drop = data_device_drop_drop,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
zxdg_toplevel_decoration_v1_handle_configure(void *data, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
	Wlwin *wl = data;
	int csd = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	if(csd == wl->client_side_deco){
		return;
	}

	wl->client_side_deco = csd;
}

static const struct zxdg_toplevel_decoration_v1_listener zxdg_toplevel_decoration_v1_listener = {
	.configure = zxdg_toplevel_decoration_v1_handle_configure,
};

static void
mode(void *data, struct wl_output*, uint, int x, int y, int)
{
	Wlwin *wl;

	wl = data;
	if(x > wl->monx || y > wl->mony){
		wl->monx = x;
		wl->mony = y;
	}
}
static void done(void*, struct wl_output*){}
static void scale(void*, struct wl_output*, int){}
static void geometry(void*, struct wl_output*, int, int, int, int, int, const char*, const char*, int){}

static const struct wl_output_listener output_listener = {
	.geometry = geometry,
	.mode = mode,
	.done = done,
	.scale = scale,
};

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	Wlwin *wl;
	struct wl_output *out;

	wl = data;
	if(strcmp(interface, wl_shm_interface.name) == 0){
		wl->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if(strcmp(interface, wl_output_interface.name) == 0){
		out = wl_registry_bind(registry, name, &wl_output_interface, 2);
		wl_output_add_listener(out, &output_listener, wl);
	} else if(strcmp(interface, wl_seat_interface.name) == 0){
		//We don't support multiseat
		if(wl->seat != nil)
			return;
		wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
		wl_seat_add_listener(wl->seat, &seat_listener, wl);
	} else if(strcmp(interface, wl_compositor_interface.name) == 0){
		wl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if(strcmp(interface, xdg_wm_base_interface.name) == 0){
		wl->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wl->xdg_wm_base, &xdg_wm_base_listener, wl);
	} else if(strcmp(interface, wl_data_device_manager_interface.name) == 0){
		wl->data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
	} else if(strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0){
		wl->decoman = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if(strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0){
		wl->primsel = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
	} else if(strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0){
		wl->vpmgr = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

void
wlsetcb(Wlwin *wl)
{
	struct wl_registry *registry;
	struct xdg_surface *xdg_surface;
	struct wl_callback *cb;
	struct zxdg_toplevel_decoration_v1 *deco;

	//Wayland doesn't do keyboard repeat, but also may
	//not tell us what the user would like, so we
	//pick some sane defaults.
	repeatstate.delay = 200;
	repeatstate.rate = 20;
	kproc("keyboard repeat", repeatproc, 0);

	registry = wl_display_get_registry(wl->display);
	wl_registry_add_listener(registry, &registry_listener, wl);
	wl_display_roundtrip(wl->display);
	wl->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	if(wl->shm == nil || wl->compositor == nil || wl->xdg_wm_base == nil || wl->seat == nil)
		panic("required wayland capabilities not met");

	if(wl->vpmgr != nil)
		wl->vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(wl->vpmgr, wl->seat);

	wlallocbuffer(wl);
	wl->surface = wl_compositor_create_surface(wl->compositor);

	xdg_surface = xdg_wm_base_get_xdg_surface(wl->xdg_wm_base, wl->surface);
	wl->xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, wl);
	xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, wl);

	wl_surface_commit(wl->surface);
	wl_display_roundtrip(wl->display);

	wl->client_side_deco = wl->decoman == nil;
	if(wl->decoman != nil){
		deco = zxdg_decoration_manager_v1_get_toplevel_decoration(wl->decoman, wl->xdg_toplevel);
		zxdg_toplevel_decoration_v1_add_listener(deco, &zxdg_toplevel_decoration_v1_listener, wl);
		zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		wl_display_roundtrip(wl->display);
	}

	xdg_toplevel_set_app_id(wl->xdg_toplevel, "drawterm");

	cb = wl_surface_frame(wl->surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, wl);

	if(wl->data_device_manager != nil && wl->seat != nil){
		wl->data_device = wl_data_device_manager_get_data_device(wl->data_device_manager, wl->seat);
		wl_data_device_add_listener(wl->data_device, &data_device_listener, wl);
		if(wl->primsel != nil)
			wl->primsel_device = zwp_primary_selection_device_manager_v1_get_device(wl->primsel, wl->seat);
		else
			iprint("primary selection not available, clipboard will not work\n");
	}
}

void
wlsettitle(Wlwin *wl, char *s)
{
	xdg_toplevel_set_title(wl->xdg_toplevel, s);
}

void
wlsetsnarf(Wlwin *wl, char *s)
{
	struct wl_data_source *source;
	struct zwp_primary_selection_source_v1 *psource;

	qlock(&wl->clip.lk);
	free(wl->clip.content);
	wl->clip.content = strdup(s);

	source = wl_data_device_manager_create_data_source(wl->data_device_manager);
	wl_data_source_add_listener(source, &data_source_listener, wl);
	wl_data_source_offer(source, "text/plain;charset=utf-8");
	wl_data_device_set_selection(wl->data_device, source, wl->clip.serial);

	psource = zwp_primary_selection_device_manager_v1_create_source(wl->primsel);
	zwp_primary_selection_source_v1_add_listener(psource, &primsel_source_listener, wl);
	zwp_primary_selection_source_v1_offer(psource, "text/plain;charset=utf-8");
	zwp_primary_selection_device_v1_set_selection(wl->primsel_device, psource, wl->clip.serial);

	qunlock(&wl->clip.lk);
}

char*
wlgetsnarf(Wlwin *wl)
{
	char *s;
	qlock(&wl->clip.lk);
	s = strdup(wl->clip.content != nil ? wl->clip.content : "");
	qunlock(&wl->clip.lk);
	return s;
}

void
wlsetmouse(Wlwin *wl, Point p)
{
	Point delta;
	if(wl->vpointer == nil)
		return;

	delta.x = p.x - wl->mouse.xy.x;
	delta.y = p.y - wl->mouse.xy.y;
	wl->mouse.xy = p;
	zwlr_virtual_pointer_v1_motion(wl->vpointer,  time(nil) * 1000, delta.x * 256, delta.y * 256);
	zwlr_virtual_pointer_v1_frame(wl->vpointer);
}
