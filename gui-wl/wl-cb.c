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
#include "xdg-decoration-protocol.h"
#include "xdg-primary-selection-protocol.h"

#include "u.h"
#include "lib.h"
#include "kern/dat.h"
#include "kern/fns.h"
#include "error.h"
#include "user.h"
#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include "screen.h"
#include "wl-inc.h"

#undef close
#undef send
#undef pipe
#undef write
#undef read

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
	Wlwin *wl;
	wl = data;
	wl->runing = 0;
	exits(nil);
}

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states)
{
	Wlwin *wl;

	wl = data;
	if(width == 0 || height == 0 || (width == wl->dx && height == wl->dy))
		return;
	wlresize(wl, width, height);
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
	wlflush(wl);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, wl);
}

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
	static struct xkb_keymap *keymap = nil;
	char *keymap_string;
	Wlwin *wl;

	wl = data;
	keymap_string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
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
}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	Wlwin *wl;
	uint32_t utf32;

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
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
		utf32 = Kalt;
		break;
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		utf32 = Kshift;
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
	default:
		utf32 = xkb_keysym_to_utf32(keysym);
		break;
	}
	if(utf32 == 0)
		return;

	if(xkb_state_mod_name_is_active(wl->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
	if(utf32 >= 'a' && utf32 <= 'z')
		utf32 -= ('a' - 1);

	kbdkey(utf32, state);
	qlock(&repeatstate.lk);
	repeatstate.active = state;
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
	WlMouse1 = 272,
	WlMouse2 = 274,
	WlMouse3 = 273,

	P9Mouse1 = 1,
	P9Mouse2 = 2,
	P9Mouse3 = 4,
};

static void
pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	Wlwin *wl;

	wl = data;
	if(state)
		switch(button){
		case WlMouse1: /* M1 */
			wl->mouse.buttons |= P9Mouse1;
			break;
		case WlMouse2: /* M2 */
			wl->mouse.buttons |= P9Mouse2;
			break;
		case WlMouse3: /* M3 */
			wl->mouse.buttons |= P9Mouse3;
			break;
		}
	else
		switch(button){
		case WlMouse1: /* M1 */
			wl->mouse.buttons &= ~P9Mouse1;
			break;
		case WlMouse2: /* M2 */
			wl->mouse.buttons &= ~P9Mouse2;
			break;
		case WlMouse3: /* M3 */
			wl->mouse.buttons &= ~P9Mouse3;
			break;
		}

	wl->mouse.msec = time;
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
	if(wl->cursorsurface == nil)
		return;
	wl_pointer_set_cursor(wl->pointer, wl->pointerserial, wl->cursorsurface, -cursor.offset.x, -cursor.offset.y);
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
	buttons = wl->mouse.buttons;
	if(value < 0){
		buttons |= 8;
	} else {
		buttons |= 16;
	}
	wl->mouse.msec = time;
	/* p9 expects a scroll event to work like a button, a set and a release */
	absmousetrack(wl->mouse.xy.x, wl->mouse.xy.y, buttons, wl->mouse.msec);
	absmousetrack(wl->mouse.xy.x, wl->mouse.xy.y, wl->mouse.buttons, wl->mouse.msec);
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

	wl = data;
	if(capabilities & WL_SEAT_CAPABILITY_POINTER) {
		wl->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
	}
	if(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, wl);
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
	wl->clip.posted = 0;
	close(fd);
	qunlock(&wl->clip.lk);
}

static void
data_source_handle_cancelled(void *data, struct wl_data_source *source)
{
	Wlwin *wl;

	wl = data;
	qlock(&wl->clip.lk);
	wl->clip.posted = 0;
	qunlock(&wl->clip.lk);
	wl_data_source_destroy(source);
}

static const struct wl_data_source_listener data_source_listener = {
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
	wl->clip.primsel_posted = 0;
	close(fd);
	qunlock(&wl->clip.lk);
}

static void
primsel_source_handle_cancelled(void *data, struct zwp_primary_selection_source_v1 *source)
{
	Wlwin *wl;

	wl = data;
	qlock(&wl->clip.lk);
	wl->clip.primsel_posted = 0;
	qunlock(&wl->clip.lk);
	zwp_primary_selection_source_v1_destroy(source);
}

static const struct zwp_primary_selection_source_v1_listener primsel_source_listener = {
	.send = primsel_source_handle_send,
	.cancelled = primsel_source_handle_cancelled,
};

static void
data_device_handle_data_offer(void *data, struct wl_data_device *data_device, struct wl_data_offer *offer)
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
	if (offer == NULL) {
		return;
	}

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
handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	Wlwin *wl;

	wl = data;
	if(strcmp(interface, wl_shm_interface.name) == 0) {
		wl->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if(strcmp(interface, wl_seat_interface.name) == 0) {
		wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
		wl_seat_add_listener(wl->seat, &seat_listener, wl);
	} else if(strcmp(interface, wl_compositor_interface.name) == 0) {
		wl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if(strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wl->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wl->xdg_wm_base, &xdg_wm_base_listener, wl);
	} else if(strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		wl->data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
	} else if(strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		wl->decoman = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if(strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
		wl->primsel = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
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

	if(wl->shm == nil || wl->compositor == nil || wl->xdg_wm_base == nil || wl->seat == nil || wl->decoman == nil || wl->primsel == nil)
		sysfatal("Registration fell short");

	wl->primsel_device = zwp_primary_selection_device_manager_v1_get_device(wl->primsel, wl->seat);

	wl->data_device = wl_data_device_manager_get_data_device(wl->data_device_manager, wl->seat);
	wl_data_device_add_listener(wl->data_device, &data_device_listener, wl);
	wlallocbuffer(wl);
	wl->surface = wl_compositor_create_surface(wl->compositor);

	xdg_surface = xdg_wm_base_get_xdg_surface(wl->xdg_wm_base, wl->surface);
	wl->xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	deco = zxdg_decoration_manager_v1_get_toplevel_decoration(wl->decoman, wl->xdg_toplevel);
	zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, wl);
	xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, wl);

	wl_surface_commit(wl->surface);
	wl_display_roundtrip(wl->display);

	xdg_toplevel_set_app_id(wl->xdg_toplevel, "drawterm");

	cb = wl_surface_frame(wl->surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, wl);
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

	/* Do we still own the clipboard? */
	if(wl->clip.posted == 0){
		source = wl_data_device_manager_create_data_source(wl->data_device_manager);
		wl_data_source_add_listener(source, &data_source_listener, wl);
		wl_data_source_offer(source, "text/plain;charset=utf-8");
		wl_data_device_set_selection(wl->data_device, source, wl->clip.serial);
		wl->clip.posted = 1;
	}

	/* Primary selection */
	if(wl->clip.primsel_posted == 0){
		psource = zwp_primary_selection_device_manager_v1_create_source(wl->primsel);
		zwp_primary_selection_source_v1_add_listener(psource, &primsel_source_listener, wl);
		zwp_primary_selection_source_v1_offer(psource, "text/plain;charset=utf-8");
		zwp_primary_selection_device_v1_set_selection(wl->primsel_device, psource, wl->clip.serial);
		wl->clip.primsel_posted = 1;
	}

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
