ROOT=..
include ../Make.config
LIB=libsec.a

OFILES=\
	aes.$O aesni.$O aesCBC.$O aes_gcm.$O\
	poly1305.$O chacha.$O chachablock.$O ccpoly.$O\
	des.$O des3CBC.$O desmodes.$O\
	ecc.$O jacobian.$O secp256k1.$O secp256r1.$O secp384r1.$O\
	curve25519.$O curve25519_dh.$O\
	genrandom.$O fastrand.$O nfastrand.$O prng.$O\
	hmac.$O hkdf.$O pbkdf2.$O\
	rsaalloc.$O rsadecrypt.$O rsaencrypt.$O\
	dh.$O\
	rc4.$O md5.$O md5block.$O\
	sha1.$O sha2_128.$O sha2_64.$O\
	sha1block.$O sha2block128.$O sha2block64.$O\
	tlshand.$O x509.$O\
	tsmemcmp.$O\

default: $(LIB)
$(LIB): $(OFILES)
	$(AR) r $(LIB) $(OFILES)
	$(RANLIB) $(LIB)

%.$O: %.c
	$(CC) $(CFLAGS) $*.c

