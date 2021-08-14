ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=\
	fbdev.$O\

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

