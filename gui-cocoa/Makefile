ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=\
	screen.$O

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

%.$O: %.m
	$(CC) $(CFLAGS) -fobjc-arc $*.m
