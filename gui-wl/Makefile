ROOT=..
include ../Make.config
LIB=libgui.a

XDG_SHELL=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
XDG_DECO=/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
XDG_PRIMSEL=/usr/share/wayland-protocols/unstable/primary-selection/primary-selection-unstable-v1.xml

HFILES=\
	xdg-shell-protocol.h\
	xdg-decoration-protocol.h\
	xdg-primary-selection-protocol.h\
	wlr-virtual-pointer.h\
	wl-inc.h\

OFILES=\
	xdg-shell-protocol.$O\
	xdg-decoration-protocol.$O\
	xdg-primary-selection-protocol.$O\
	wlr-virtual-pointer.$O\
	wl-cb.$O\
	wl-screen.$O\
	wl-util.$O\

wlr-virtual-pointer-unstable-v1.xml:
	curl https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-virtual-pointer-unstable-v1.xml > $@

wlr-virtual-pointer.c: wlr-virtual-pointer-unstable-v1.xml
	wayland-scanner private-code < wlr-virtual-pointer-unstable-v1.xml > $@ || { rm -f $@; exit 1; }

wlr-virtual-pointer.h: wlr-virtual-pointer-unstable-v1.xml
	wayland-scanner client-header < wlr-virtual-pointer-unstable-v1.xml > $@ || { rm -f $@; exit 1; }

xdg-shell-protocol.c:
	wayland-scanner private-code < $(XDG_SHELL) > $@ || { rm -f $@; exit 1; }

xdg-shell-protocol.h:
	wayland-scanner client-header < $(XDG_SHELL) > $@ || { rm -f $@; exit 1; }

xdg-decoration-protocol.c:
	wayland-scanner private-code < $(XDG_DECO) > $@ || { rm -f $@; exit 1; }

xdg-decoration-protocol.h:
	wayland-scanner client-header < $(XDG_DECO) > $@ || { rm -f $@; exit 1; }

xdg-primary-selection-protocol.c:
	wayland-scanner private-code < $(XDG_PRIMSEL) > $@ || { rm -f $@; exit 1; }

xdg-primary-selection-protocol.h:
	wayland-scanner client-header < $(XDG_PRIMSEL) > $@ || { rm -f $@; exit 1; }

wl-cb.$O: $(HFILES)

default: $(LIB)
$(LIB): $(HFILES) $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

