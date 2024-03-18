ROOT=..
include ../Make.config
LIB=libgui.a

OFILES=screen.$O

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

screen.$O: screen.cpp
	$(CXX) -o $@ $(CFLAGS) $<
