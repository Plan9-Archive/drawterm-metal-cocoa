ROOT=.

include Make.config

OFILES=\
	main.$O\
	cpu.$O\
	aan.$O\
	secstore.$O\
	latin1.$O\
	$(OS)-factotum.$O\
	$(XOFILES)\

LIBS1=\
	kern/libkern.a\
	exportfs/libexportfs.a\
	libauth/libauth.a\
	libauthsrv/libauthsrv.a\
	libsec/libsec.a\
	libmp/libmp.a\
	libmemdraw/libmemdraw.a\
	libmemlayer/libmemlayer.a\
	libdraw/libdraw.a\
	gui-$(GUI)/libgui.a\
	libc/libc.a\
	libip/libip.a\

# stupid gcc
LIBS=$(LIBS1) $(LIBS1) $(LIBS1) libmachdep.a

default: $(TARG)
$(TARG): $(OFILES) $(LIBS)
	$(CC) $(LDFLAGS) -o $(TARG) $(OFILES) $(LIBS) $(LDADD)

%.$O: %.c
	$(CC) $(CFLAGS) $*.c

clean:
	rm -f *.o */*.o */*.a *.a drawterm drawterm.exe

force:

kern/libkern.a:	force
	(cd kern; $(MAKE))

exportfs/libexportfs.a:	force
	(cd exportfs; $(MAKE))

libauth/libauth.a:	force
	(cd libauth; $(MAKE))
	
libauthsrv/libauthsrv.a:	force
	(cd libauthsrv; $(MAKE))

libmp/libmp.a:	force
	(cd libmp; $(MAKE))

libsec/libsec.a:	force
	(cd libsec; $(MAKE))

libmemdraw/libmemdraw.a:	force
	(cd libmemdraw; $(MAKE))

libmemlayer/libmemlayer.a:	force
	(cd libmemlayer; $(MAKE))

libdraw/libdraw.a:	force
	(cd libdraw; $(MAKE))

libc/libc.a:	force
	(cd libc; $(MAKE))

libip/libip.a:	force
	(cd libip; $(MAKE))

gui-$(GUI)/libgui.a:	force
	(cd gui-$(GUI); $(MAKE))
