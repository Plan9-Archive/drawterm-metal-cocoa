ROOT=..
include ../Make.config
LIB=../libmachdep.a

OFILES=\
	getcallerpc.$O\
	md5block.$O\
	sha1block.$O\
	tas.$O

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

tas.$O: tas.s
	ln -sf tas.s tas.S
	$(CC) -c -o tas.$O -mips3 tas.S
