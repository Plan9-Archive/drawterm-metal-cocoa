ROOT=..
include ../Make.config
LIB=libgui.a

XDG_SHELL=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
XDG_DECO=/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

HFILES=\
	xdg-shell-protocol.h\
	xdg-decoration-protocol.h\
	wl-inc.h\

OFILES=\
	xdg-shell-protocol.$O\
	xdg-decoration-protocol.$O\
	wl-cb.$O\
	wl-screen.$O\
	wl-util.$O\

xdg-shell-protocol.c:
	wayland-scanner private-code < $(XDG_SHELL) > $@ || { rm -f $@; exit 1; }

xdg-shell-protocol.h:
	wayland-scanner client-header < $(XDG_SHELL) > $@ || { rm -f $@; exit 1; }

xdg-decoration-protocol.c:
	wayland-scanner private-code < $(XDG_DECO) > $@ || { rm -f $@; exit 1; }

xdg-decoration-protocol.h:
	wayland-scanner client-header < $(XDG_DECO) > $@ || { rm -f $@; exit 1; }

default: $(LIB)
$(LIB): $(HFILES) $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

