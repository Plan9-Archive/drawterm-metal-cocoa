ROOT=..
include ../Make.config

LIB=libauth.a
OFILES=\
	attr.$O\
	auth_attr.$O\
	auth_proxy.$O\
	auth_rpc.$O\

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

%.$O: %.c
	$(CC) $(CFLAGS) $*.c

