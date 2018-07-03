ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=\
	screen.$O

default: $(LIB) drawterm.app/MainWindow.nib
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

%.$O: %.m
	$(CC) $(CFLAGS) -fobjc-arc $*.m

drawterm.app/MainWindow.nib: MainWindow.xib
	ibtool --compile $@ $<
