ROOT=..
include ../Make.config
LIB=libkern.a

OFILES=\
	alloc.$O\
	allocb.$O\
	chan.$O\
	data.$O\
	dev.$O\
	devaudio.$O\
	devaudio-$(AUDIO).$O\
	devcmd.$O\
	devcons.$O\
	devdraw.$O\
	devenv.$O\
	devfs-$(OS).$O\
	devip.$O\
	devip-$(OS).$O\
	devkbd.$O\
	devlfd-$(OS).$O\
	devmnt.$O\
	devmouse.$O\
	devpipe.$O\
	devroot.$O\
	devssl.$O\
	devtls.$O\
	devtab.$O\
	error.$O\
	parse.$O\
	pgrp.$O\
	procinit.$O\
	rwlock.$O\
	sleep.$O\
	stub.$O\
	sysfile.$O\
	qio.$O\
	qlock.$O\
	term.$O\
	waserror.$O\
	$(OS).$O

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

%.$O: %.c
	$(CC) $(CFLAGS) $*.c

devaudio-haiku.$O: devaudio-haiku.cpp
	$(CXX) $(CFLAGS) $<
