ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=\
	screen.$O\
	r16.$O

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

%.$O: %.c
	$(CC) $(CFLAGS) $*.c

