#include <u.h>
#include <libc.h>
#include <auth.h>
#include <mp.h>
#include <libsec.h>

// The main groups of functions are:
//		client/server - main handshake protocol definition
//		message functions - formating handshake messages
//		cipher choices - catalog of digest and encrypt algorithms
//		security functions - PKCS#1, sslHMAC, session keygen
//		general utility functions - malloc, serialization
// The handshake protocol builds on the TLS/SSL3 record layer protocol,
// which is implemented in kernel device #a.  See also /lib/rfc/rfc2246.

enum {
	TLSFinishedLen = 12,
	SSL3FinishedLen = MD5dlen+SHA1dlen,
	MaxKeyData = 160,	// amount of secret we may need
	MAXdlen = SHA2_512dlen,
	RandomSize = 32,
	MasterSecretSize = 48,
	AQueue = 0,
	AFlush = 1,
};

typedef struct Bytes{
	int len;
	uchar data[];
} Bytes;

typedef struct Ints{
	int len;
	int data[];
} Ints;

typedef struct Algs{
	char *enc;
	char *digest;
	int nsecret;
	int tlsid;
	int ok;
} Algs;

typedef struct Namedcurve{
	int tlsid;
	void (*init)(mpint *p, mpint *a, mpint *b, mpint *x, mpint *y, mpint *n, mpint *h);
} Namedcurve;

typedef struct Finished{
	uchar verify[SSL3FinishedLen];
	int n;
} Finished;

typedef struct HandshakeHash {
	MD5state	md5;
	SHAstate	sha1;
	SHA2_256state	sha2_256;
} HandshakeHash;

typedef struct TlsSec TlsSec;
struct TlsSec {
	RSApub *rsapub;
	AuthRpc *rpc;	// factotum for rsa private key
	uchar *psk;	// pre-shared key
	int psklen;
	int clientVers;			// version in ClientHello
	uchar sec[MasterSecretSize];	// master secret
	uchar srandom[RandomSize];	// server random
	uchar crandom[RandomSize];	// client random

	Namedcurve *nc; // selected curve for ECDHE
	// diffie hellman state
	DHstate dh;
	struct {
		ECdomain dom;
		ECpriv Q;
	} ec;
	uchar X[32];

	// byte generation and handshake checksum
	void (*prf)(uchar*, int, uchar*, int, char*, uchar*, int);
	void (*setFinished)(TlsSec*, HandshakeHash, uchar*, int);
	int nfin;
};

typedef struct TlsConnection{
	TlsSec sec[1];	// security management goo
	int hand, ctl;	// record layer file descriptors
	int erred;		// set when tlsError called
	int (*trace)(char*fmt, ...); // for debugging
	int version;	// protocol we are speaking
	Bytes *cert;	// server certificate; only last - no chain

	int cipher;
	int nsecret;	// amount of secret data to init keys
	char *digest;	// name of digest algorithm to use
	char *enc;	// name of encryption algorithm to use

	// for finished messages
	HandshakeHash	handhash;
	Finished	finished;

	uchar *sendp;
	uchar buf[1<<16];
} TlsConnection;

typedef struct Msg{
	int tag;
	union {
		struct {
			int	version;
			uchar 	random[RandomSize];
			Bytes*	sid;
			Ints*	ciphers;
			Bytes*	compressors;
			Bytes*	extensions;
		} clientHello;
		struct {
			int	version;
			uchar	random[RandomSize];
			Bytes*	sid;
			int	cipher;
			int	compressor;
			Bytes*	extensions;
		} serverHello;
		struct {
			int ncert;
			Bytes **certs;
		} certificate;
		struct {
			Bytes *types;
			Ints *sigalgs;
			int nca;
			Bytes **cas;
		} certificateRequest;
		struct {
			Bytes *pskid;
			Bytes *key;
		} clientKeyExchange;
		struct {
			Bytes *pskid;
			Bytes *dh_p;
			Bytes *dh_g;
			Bytes *dh_Ys;
			Bytes *dh_parameters;
			Bytes *dh_signature;
			int sigalg;
			int curve;
		} serverKeyExchange;
		struct {
			int sigalg;
			Bytes *signature;
		} certificateVerify;		
		Finished finished;
	} u;
} Msg;


enum {
	SSL3Version	= 0x0300,
	TLS10Version	= 0x0301,
	TLS11Version	= 0x0302,
	TLS12Version	= 0x0303,
	ProtocolVersion	= TLS12Version,	// maximum version we speak
	MinProtoVersion	= 0x0300,	// limits on version we accept
	MaxProtoVersion	= 0x03ff,
};

// handshake type
enum {
	HHelloRequest,
	HClientHello,
	HServerHello,
	HSSL2ClientHello = 9,  /* local convention;  see devtls.c */
	HCertificate = 11,
	HServerKeyExchange,
	HCertificateRequest,
	HServerHelloDone,
	HCertificateVerify,
	HClientKeyExchange,
	HFinished = 20,
	HMax
};

// alerts
enum {
	ECloseNotify = 0,
	EUnexpectedMessage = 10,
	EBadRecordMac = 20,
	EDecryptionFailed = 21,
	ERecordOverflow = 22,
	EDecompressionFailure = 30,
	EHandshakeFailure = 40,
	ENoCertificate = 41,
	EBadCertificate = 42,
	EUnsupportedCertificate = 43,
	ECertificateRevoked = 44,
	ECertificateExpired = 45,
	ECertificateUnknown = 46,
	EIllegalParameter = 47,
	EUnknownCa = 48,
	EAccessDenied = 49,
	EDecodeError = 50,
	EDecryptError = 51,
	EExportRestriction = 60,
	EProtocolVersion = 70,
	EInsufficientSecurity = 71,
	EInternalError = 80,
	EInappropriateFallback = 86,
	EUserCanceled = 90,
	ENoRenegotiation = 100,
	EUnknownPSKidentity = 115,
	EMax = 256
};

// cipher suites
enum {
	TLS_RSA_WITH_3DES_EDE_CBC_SHA		= 0X000A,
	TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA	= 0X0016,

	TLS_RSA_WITH_AES_128_CBC_SHA		= 0X002F,
	TLS_DHE_RSA_WITH_AES_128_CBC_SHA	= 0X0033,
	TLS_RSA_WITH_AES_256_CBC_SHA		= 0X0035,
	TLS_DHE_RSA_WITH_AES_256_CBC_SHA	= 0X0039,
	TLS_RSA_WITH_AES_128_CBC_SHA256		= 0X003C,
	TLS_RSA_WITH_AES_256_CBC_SHA256		= 0X003D,
	TLS_DHE_RSA_WITH_AES_128_CBC_SHA256	= 0X0067,

	TLS_RSA_WITH_AES_128_GCM_SHA256		= 0x009C,
	TLS_DHE_RSA_WITH_AES_128_GCM_SHA256	= 0x009E,

	TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA	= 0xC013,
	TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA	= 0xC014,
	TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256	= 0xC023,
	TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256	= 0xC027,

	TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 = 0xC02B,
	TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256	= 0xC02F,

	GOOGLE_ECDHE_RSA_WITH_CHACHA20_POLY1305		= 0xCC13,
	GOOGLE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305	= 0xCC14,
	GOOGLE_DHE_RSA_WITH_CHACHA20_POLY1305		= 0xCC15,

	TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305	= 0xCCA8,
	TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305	= 0xCCA9,
	TLS_DHE_RSA_WITH_CHACHA20_POLY1305	= 0xCCAA,

	TLS_PSK_WITH_CHACHA20_POLY1305		= 0xCCAB,
	TLS_PSK_WITH_AES_128_CBC_SHA256		= 0x00AE,
	TLS_PSK_WITH_AES_128_CBC_SHA		= 0x008C,

	TLS_FALLBACK_SCSV = 0x5600,
};

// compression methods
enum {
	CompressionNull = 0,
	CompressionMax
};


// curves
enum {
	X25519 = 0x001d,
};

// extensions
enum {
	Extsni = 0x0000,
	Extec = 0x000a,
	Extecp = 0x000b,
	Extsigalgs = 0x000d,
};

static Algs cipherAlgs[] = {
	// ECDHE-ECDSA
	{"ccpoly96_aead", "clear", 2*(32+12), TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305, 0},
	{"ccpoly64_aead", "clear", 2*32, GOOGLE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305, 0},
	{"aes_128_gcm_aead", "clear", 2*(16+4), TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0},
	{"aes_128_cbc", "sha256", 2*(16+16+SHA2_256dlen), TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, 0},

	// ECDHE-RSA
	{"ccpoly96_aead", "clear", 2*(32+12), TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305, 0},
	{"ccpoly64_aead", "clear", 2*32, GOOGLE_ECDHE_RSA_WITH_CHACHA20_POLY1305, 0},
	{"aes_128_gcm_aead", "clear", 2*(16+4), TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0},
	{"aes_128_cbc", "sha256", 2*(16+16+SHA2_256dlen), TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256, 0},
	{"aes_128_cbc", "sha1", 2*(16+16+SHA1dlen), TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA, 0},
	{"aes_256_cbc", "sha1", 2*(32+16+SHA1dlen), TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA, 0},

	// DHE-RSA
	{"ccpoly96_aead", "clear", 2*(32+12), TLS_DHE_RSA_WITH_CHACHA20_POLY1305, 0},
	{"ccpoly64_aead", "clear", 2*32, GOOGLE_DHE_RSA_WITH_CHACHA20_POLY1305, 0},
	{"aes_128_gcm_aead", "clear", 2*(16+4), TLS_DHE_RSA_WITH_AES_128_GCM_SHA256, 0},
	{"aes_128_cbc", "sha256", 2*(16+16+SHA2_256dlen), TLS_DHE_RSA_WITH_AES_128_CBC_SHA256, 0},
	{"aes_128_cbc", "sha1", 2*(16+16+SHA1dlen), TLS_DHE_RSA_WITH_AES_128_CBC_SHA, 0},
	{"aes_256_cbc", "sha1", 2*(32+16+SHA1dlen), TLS_DHE_RSA_WITH_AES_256_CBC_SHA, 0},
	{"3des_ede_cbc","sha1",	2*(4*8+SHA1dlen), TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA, 0},

	// RSA
	{"aes_128_gcm_aead", "clear", 2*(16+4), TLS_RSA_WITH_AES_128_GCM_SHA256, 0},
	{"aes_128_cbc", "sha256", 2*(16+16+SHA2_256dlen), TLS_RSA_WITH_AES_128_CBC_SHA256, 0},
	{"aes_256_cbc", "sha256", 2*(32+16+SHA2_256dlen), TLS_RSA_WITH_AES_256_CBC_SHA256, 0},
	{"aes_128_cbc", "sha1", 2*(16+16+SHA1dlen), TLS_RSA_WITH_AES_128_CBC_SHA, 0},
	{"aes_256_cbc", "sha1", 2*(32+16+SHA1dlen), TLS_RSA_WITH_AES_256_CBC_SHA, 0},
	{"3des_ede_cbc","sha1",	2*(4*8+SHA1dlen), TLS_RSA_WITH_3DES_EDE_CBC_SHA, 0},

	// PSK
	{"ccpoly96_aead", "clear", 2*(32+12), TLS_PSK_WITH_CHACHA20_POLY1305, 0},
	{"aes_128_cbc", "sha256", 2*(16+16+SHA2_256dlen), TLS_PSK_WITH_AES_128_CBC_SHA256, 0},
	{"aes_128_cbc", "sha1", 2*(16+16+SHA1dlen), TLS_PSK_WITH_AES_128_CBC_SHA, 0},
};

static uchar compressors[] = {
	CompressionNull,
};

static Namedcurve namedcurves[] = {
	X25519, nil,
	0x0017, secp256r1,
	0x0018, secp384r1,
};

static uchar pointformats[] = {
	CompressionNull /* support of uncompressed point format is mandatory */
};

static struct {
	DigestState* (*fun)(uchar*, ulong, uchar*, DigestState*);
	int len;
} hashfun[] = {
/*	[0x00]  is reserved for MD5+SHA1 for < TLS1.2 */
	[0x01] = {md5,		MD5dlen},
	[0x02] = {sha1,		SHA1dlen},
	[0x03] = {sha2_224,	SHA2_224dlen},
	[0x04] = {sha2_256,	SHA2_256dlen},
	[0x05] = {sha2_384,	SHA2_384dlen},
	[0x06] = {sha2_512,	SHA2_512dlen},
};

// signature algorithms (only RSA and ECDSA at the moment)
static int sigalgs[] = {
	0x0603,		/* SHA512 ECDSA */
	0x0503,		/* SHA384 ECDSA */
	0x0403,		/* SHA256 ECDSA */
	0x0203,		/* SHA1 ECDSA */

	0x0601,		/* SHA512 RSA */
	0x0501,		/* SHA384 RSA */
	0x0401,		/* SHA256 RSA */
	0x0201,		/* SHA1 RSA */
};

static TlsConnection *tlsServer2(int ctl, int hand,
	uchar *cert, int certlen,
	char *pskid, uchar *psk, int psklen,
	int (*trace)(char*fmt, ...), PEMChain *chain);
static TlsConnection *tlsClient2(int ctl, int hand,
	uchar *cert, int certlen,
	char *pskid, uchar *psk, int psklen,
	uchar *ext, int extlen, int (*trace)(char*fmt, ...));
static void	msgClear(Msg *m);
static char* msgPrint(char *buf, int n, Msg *m);
static int	msgRecv(TlsConnection *c, Msg *m);
static int	msgSend(TlsConnection *c, Msg *m, int act);
static void	tlsError(TlsConnection *c, int err, char *msg, ...);
static int setVersion(TlsConnection *c, int version);
static int setSecrets(TlsConnection *c, int isclient);
static int finishedMatch(TlsConnection *c, Finished *f);
static void tlsConnectionFree(TlsConnection *c);

static int isDHE(int tlsid);
static int isECDHE(int tlsid);
static int isPSK(int tlsid);
static int isECDSA(int tlsid);

static int setAlgs(TlsConnection *c, int a);
static int okCipher(Ints *cv, int ispsk, int canec);
static int okCompression(Bytes *cv);
static int initCiphers(void);
static Ints* makeciphers(int ispsk);

static AuthRpc* factotum_rsa_open(RSApub *rsapub);
static mpint* factotum_rsa_decrypt(AuthRpc *rpc, mpint *cipher);
static void factotum_rsa_close(AuthRpc *rpc);

static void	tlsSecInits(TlsSec *sec, int cvers, uchar *crandom);
static int	tlsSecRSAs(TlsSec *sec, Bytes *epm);
static Bytes*	tlsSecECDHEs1(TlsSec *sec);
static int	tlsSecECDHEs2(TlsSec *sec, Bytes *Yc);
static void	tlsSecInitc(TlsSec *sec, int cvers);
static Bytes*	tlsSecRSAc(TlsSec *sec, uchar *cert, int ncert);
static Bytes*	tlsSecDHEc(TlsSec *sec, Bytes *p, Bytes *g, Bytes *Ys);
static Bytes*	tlsSecECDHEc(TlsSec *sec, int curve, Bytes *Ys);
static void	tlsSecVers(TlsSec *sec, int v);
static int	tlsSecFinished(TlsSec *sec, HandshakeHash hsh, uchar *fin, int nfin, int isclient);
static void	setMasterSecret(TlsSec *sec, Bytes *pm);
static int	digestDHparams(TlsSec *sec, Bytes *par, uchar digest[MAXdlen], int sigalg);
static char*	verifyDHparams(TlsSec *sec, Bytes *par, Bytes *cert, Bytes *sig, int sigalg);

static Bytes*	pkcs1_encrypt(Bytes* data, RSApub* key);
static Bytes*	pkcs1_decrypt(TlsSec *sec, Bytes *data);
static Bytes*	pkcs1_sign(TlsSec *sec, uchar *digest, int digestlen, int sigalg);

static void* emalloc(int);
static void* erealloc(void*, int);
// static void put32(uchar *p, u32int);
static void put24(uchar *p, int);
static void put16(uchar *p, int);
static int get24(uchar *p);
static int get16(uchar *p);
static Bytes* newbytes(int len);
static Bytes* makebytes(uchar* buf, int len);
static Bytes* mptobytes(mpint* big, int len);
static mpint* bytestomp(Bytes* bytes);
static void freebytes(Bytes* b);
static Ints* newints(int len);
static void freeints(Ints* b);
static int lookupid(Ints* b, int id);

/* x509.c */
extern mpint*	pkcs1padbuf(uchar *buf, int len, mpint *modulus, int blocktype);
extern int	pkcs1unpadbuf(uchar *buf, int len, mpint *modulus, int blocktype);
extern int	asn1encodedigest(DigestState* (*fun)(uchar*, ulong, uchar*, DigestState*), uchar *digest, uchar *buf, int len);

//================= client/server ========================

//	push TLS onto fd, returning new (application) file descriptor
//		or -1 if error.
int
tlsServer(int fd, TLSconn *conn)
{
	char buf[8];
	char dname[64];
	uchar seed[2*RandomSize];
	int n, data, ctl, hand;
	TlsConnection *tls;

	if(conn == nil)
		return -1;
	ctl = open("#a/tls/clone", ORDWR|OCEXEC);
	if(ctl < 0)
		return -1;
	n = read(ctl, buf, sizeof(buf)-1);
	if(n < 0){
		close(ctl);
		return -1;
	}
	buf[n] = 0;
	snprint(conn->dir, sizeof(conn->dir), "#a/tls/%s", buf);
	snprint(dname, sizeof(dname), "#a/tls/%s/hand", buf);
	hand = open(dname, ORDWR|OCEXEC);
	if(hand < 0){
		close(ctl);
		return -1;
	}
	data = -1;
	fprint(ctl, "fd %d 0x%x", fd, ProtocolVersion);
	tls = tlsServer2(ctl, hand,
		conn->cert, conn->certlen,
		conn->pskID, conn->psk, conn->psklen,
		conn->trace, conn->chain);
	if(tls != nil){
		snprint(dname, sizeof(dname), "#a/tls/%s/data", buf);
		data = open(dname, ORDWR);
	}
	close(hand);
	close(ctl);
	if(data < 0){
		tlsConnectionFree(tls);
		return -1;
	}
	free(conn->cert);
	conn->cert = nil;  // client certificates are not yet implemented
	conn->certlen = 0;
	conn->sessionIDlen = 0;
	conn->sessionID = nil;
	if(conn->sessionKey != nil
	&& conn->sessionType != nil
	&& strcmp(conn->sessionType, "ttls") == 0){
		memmove(seed, tls->sec->crandom, RandomSize);
		memmove(seed+RandomSize, tls->sec->srandom, RandomSize);
		tls->sec->prf(
			conn->sessionKey, conn->sessionKeylen,
			tls->sec->sec, MasterSecretSize,
			conn->sessionConst, 
			seed, sizeof(seed));
	}
	tlsConnectionFree(tls);
	close(fd);
	return data;
}

static uchar*
tlsClientExtensions(TLSconn *conn, int *plen)
{
	uchar *b, *p;
	int i, n, m;

	p = b = nil;

	// RFC6066 - Server Name Identification
	if(conn->serverName != nil && (n = strlen(conn->serverName)) > 0){
		m = p - b;
		b = erealloc(b, m + 2+2+2+1+2+n);
		p = b + m;

		put16(p, Extsni), p += 2;	/* Type: server_name */
		put16(p, 2+1+2+n), p += 2;	/* Length */
		put16(p, 1+2+n), p += 2;	/* Server Name list length */
		*p++ = 0;			/* Server Name Type: host_name */
		put16(p, n), p += 2;		/* Server Name length */
		memmove(p, conn->serverName, n);
		p += n;
	}

	// Elliptic Curves (also called Supported Groups)
	if(ProtocolVersion >= TLS10Version){
		m = p - b;
		b = erealloc(b, m + 2+2+2+nelem(namedcurves)*2 + 2+2+1+nelem(pointformats));
		p = b + m;

		n = nelem(namedcurves);
		put16(p, Extec), p += 2;	/* Type: elliptic_curves / supported_groups */
		put16(p, (n+1)*2), p += 2;	/* Length */
		put16(p, n*2), p += 2;		/* Elliptic Curves Length */
		for(i=0; i < n; i++){		/* Elliptic Curves */
			put16(p, namedcurves[i].tlsid);
			p += 2;
		}

		n = nelem(pointformats);
		put16(p, Extecp), p += 2;	/* Type: ec_point_formats */
		put16(p, n+1), p += 2;		/* Length */
		*p++ = n;			/* EC point formats Length */
		for(i=0; i < n; i++)		/* EC point formats */
			*p++ = pointformats[i];
	}

	// signature algorithms
	if(ProtocolVersion >= TLS12Version){
		n = nelem(sigalgs);

		m = p - b;
		b = erealloc(b, m + 2+2+2+n*2);
		p = b + m;

		put16(p, Extsigalgs), p += 2;
		put16(p, n*2 + 2), p += 2;
		put16(p, n*2), p += 2;
		for(i=0; i < n; i++){
			put16(p, sigalgs[i]);
			p += 2;
		}
	}
	
	*plen = p - b;
	return b;
}

//	push TLS onto fd, returning new (application) file descriptor
//		or -1 if error.
int
tlsClient(int fd, TLSconn *conn)
{
	char buf[8];
	char dname[64];
	uchar seed[2*RandomSize];
	int n, data, ctl, hand;
	TlsConnection *tls;
	uchar *ext;

	if(conn == nil)
		return -1;
	ctl = open("#a/tls/clone", ORDWR|OCEXEC);
	if(ctl < 0)
		return -1;
	n = read(ctl, buf, sizeof(buf)-1);
	if(n < 0){
		close(ctl);
		return -1;
	}
	buf[n] = 0;
	snprint(conn->dir, sizeof(conn->dir), "#a/tls/%s", buf);
	snprint(dname, sizeof(dname), "#a/tls/%s/hand", buf);
	hand = open(dname, ORDWR|OCEXEC);
	if(hand < 0){
		close(ctl);
		return -1;
	}
	snprint(dname, sizeof(dname), "#a/tls/%s/data", buf);
	data = open(dname, ORDWR);
	if(data < 0){
		close(hand);
		close(ctl);
		return -1;
	}
	fprint(ctl, "fd %d 0x%x", fd, ProtocolVersion);
	ext = tlsClientExtensions(conn, &n);
	tls = tlsClient2(ctl, hand,
		conn->cert, conn->certlen, 
		conn->pskID, conn->psk, conn->psklen,
		ext, n, conn->trace);
	free(ext);
	close(hand);
	close(ctl);
	if(tls == nil){
		close(data);
		return -1;
	}
	free(conn->cert);
	if(tls->cert != nil){
		conn->certlen = tls->cert->len;
		conn->cert = emalloc(conn->certlen);
		memcpy(conn->cert, tls->cert->data, conn->certlen);
	} else {
		conn->certlen = 0;
		conn->cert = nil;
	}
	conn->sessionIDlen = 0;
	conn->sessionID = nil;
	if(conn->sessionKey != nil
	&& conn->sessionType != nil
	&& strcmp(conn->sessionType, "ttls") == 0){
		memmove(seed, tls->sec->crandom, RandomSize);
		memmove(seed+RandomSize, tls->sec->srandom, RandomSize);
		tls->sec->prf(
			conn->sessionKey, conn->sessionKeylen,
			tls->sec->sec, MasterSecretSize,
			conn->sessionConst, 
			seed, sizeof(seed));
	}
	tlsConnectionFree(tls);
	close(fd);
	return data;
}

static int
countchain(PEMChain *p)
{
	int i = 0;

	while (p) {
		i++;
		p = p->next;
	}
	return i;
}

static int
checkClientExtensions(TlsConnection *c, Bytes *ext)
{
	uchar *p, *e;
	uint i, j, n;

	if(ext == nil)
		return 0;

	for(p = ext->data, e = p+ext->len; p < e; p += n){
		if(e-p < 4)
			goto Short;
		p += 4;
		if(e-p < (n = get16(p-2)))
			goto Short;
		switch(get16(p-4)){
		case Extec:
			if(n < 4 || n % 2 || (uint)get16(p) != (n -= 2))
				goto Short;
			p += 2;
			for(i = 0; i < nelem(namedcurves) && c->sec->nc == nil; i++)
				for(j = 0; j < n; j += 2)
					if(namedcurves[i].tlsid == get16(p+j)){
						c->sec->nc = &namedcurves[i];
						break;
					}
			break;
		}
	}

	return 0;
Short:
	tlsError(c, EDecodeError, "clienthello extensions has invalid length");
	return -1; 
} 

static TlsConnection *
tlsServer2(int ctl, int hand,
	uchar *cert, int certlen,
	char *pskid, uchar *psk, int psklen,
	int (*trace)(char*fmt, ...), PEMChain *chp)
{
	int cipher, compressor, numcerts, i;
	TlsConnection *c;
	Msg m;

	if(trace)
		trace("tlsServer2\n");
	if(!initCiphers())
		return nil;

	c = emalloc(sizeof(TlsConnection));
	c->ctl = ctl;
	c->hand = hand;
	c->trace = trace;
	c->version = ProtocolVersion;
	c->sendp = c->buf;

	memset(&m, 0, sizeof(m));
	if(!msgRecv(c, &m)){
		if(trace)
			trace("initial msgRecv failed\n");
		goto Err;
	}
	if(m.tag != HClientHello) {
		tlsError(c, EUnexpectedMessage, "expected a client hello");
		goto Err;
	}
	if(trace)
		trace("ClientHello version %x\n", m.u.clientHello.version);
	if(setVersion(c, m.u.clientHello.version) < 0) {
		tlsError(c, EIllegalParameter, "incompatible version");
		goto Err;
	}
	if(c->version < ProtocolVersion
	&& lookupid(m.u.clientHello.ciphers, TLS_FALLBACK_SCSV) >= 0){
		tlsError(c, EInappropriateFallback, "inappropriate fallback");
		goto Err;
	}
	tlsSecInits(c->sec, m.u.clientHello.version, m.u.clientHello.random);
	tlsSecVers(c->sec, c->version);
	if(psklen > 0){
		c->sec->psk = psk;
		c->sec->psklen = psklen;
	}
	if(certlen > 0){
		/* server certificate */
		c->sec->rsapub = X509toRSApub(cert, certlen, nil, 0);
		if(c->sec->rsapub == nil){
			tlsError(c, EHandshakeFailure, "invalid X509/rsa certificate");
			goto Err;
		}
		c->sec->rpc = factotum_rsa_open(c->sec->rsapub);
		if(c->sec->rpc == nil){
			tlsError(c, EHandshakeFailure, "factotum_rsa_open: %r");
			goto Err;
		}
	}
	if(checkClientExtensions(c, m.u.clientHello.extensions) < 0)
		goto Err;
	cipher = okCipher(m.u.clientHello.ciphers, psklen > 0, c->sec->nc != nil);
	if(cipher < 0 || !setAlgs(c, cipher)) {
		tlsError(c, EHandshakeFailure, "no matching cipher suite");
		goto Err;
	}
	compressor = okCompression(m.u.clientHello.compressors);
	if(compressor < 0) {
		tlsError(c, EHandshakeFailure, "no matching compressor");
		goto Err;
	}
	if(trace)
		trace("  cipher %x, compressor %x\n", cipher, compressor);
	msgClear(&m);

	m.tag = HServerHello;
	m.u.serverHello.version = c->version;
	memmove(m.u.serverHello.random, c->sec->srandom, RandomSize);
	m.u.serverHello.cipher = cipher;
	m.u.serverHello.compressor = compressor;
	m.u.serverHello.sid = makebytes(nil, 0);
	if(!msgSend(c, &m, AQueue))
		goto Err;

	if(certlen > 0){
		m.tag = HCertificate;
		numcerts = countchain(chp);
		m.u.certificate.ncert = 1 + numcerts;
		m.u.certificate.certs = emalloc(m.u.certificate.ncert * sizeof(Bytes*));
		m.u.certificate.certs[0] = makebytes(cert, certlen);
		for (i = 0; i < numcerts && chp; i++, chp = chp->next)
			m.u.certificate.certs[i+1] = makebytes(chp->pem, chp->pemlen);
		if(!msgSend(c, &m, AQueue))
			goto Err;
	}

	if(isECDHE(cipher)){
		m.tag = HServerKeyExchange;
		m.u.serverKeyExchange.curve = c->sec->nc->tlsid;
		m.u.serverKeyExchange.dh_parameters = tlsSecECDHEs1(c->sec);
		if(m.u.serverKeyExchange.dh_parameters == nil){
			tlsError(c, EInternalError, "can't set DH parameters");
			goto Err;
		}

		/* sign the DH parameters */
		if(certlen > 0){
			uchar digest[MAXdlen];
			int digestlen;

			if(c->version >= TLS12Version)
				m.u.serverKeyExchange.sigalg = 0x0401;	/* RSA SHA256 */
			digestlen = digestDHparams(c->sec, m.u.serverKeyExchange.dh_parameters,
				digest, m.u.serverKeyExchange.sigalg);
			if((m.u.serverKeyExchange.dh_signature = pkcs1_sign(c->sec, digest, digestlen,
				m.u.serverKeyExchange.sigalg)) == nil){
				tlsError(c, EHandshakeFailure, "pkcs1_sign: %r");
				goto Err;
			}
		}
		if(!msgSend(c, &m, AQueue))
			goto Err;
	}

	m.tag = HServerHelloDone;
	if(!msgSend(c, &m, AFlush))
		goto Err;

	if(!msgRecv(c, &m))
		goto Err;
	if(m.tag != HClientKeyExchange) {
		tlsError(c, EUnexpectedMessage, "expected a client key exchange");
		goto Err;
	}
	if(pskid != nil){
		if(m.u.clientKeyExchange.pskid == nil
		|| (unsigned long)m.u.clientKeyExchange.pskid->len != strlen(pskid)
		|| memcmp(pskid, m.u.clientKeyExchange.pskid->data, m.u.clientKeyExchange.pskid->len) != 0){
			tlsError(c, EUnknownPSKidentity, "unknown or missing pskid");
			goto Err;
		}
	}
	if(isECDHE(cipher)){
		if(tlsSecECDHEs2(c->sec, m.u.clientKeyExchange.key) < 0){
			tlsError(c, EHandshakeFailure, "couldn't set keys: %r");
			goto Err;
		}
	} else if(certlen > 0){
		if(tlsSecRSAs(c->sec, m.u.clientKeyExchange.key) < 0){
			tlsError(c, EHandshakeFailure, "couldn't set keys: %r");
			goto Err;
		}
	} else if(psklen > 0){
		setMasterSecret(c->sec, newbytes(psklen));
	} else {
		tlsError(c, EInternalError, "no psk or certificate");
		goto Err;
	}

	if(trace)
		trace("tls secrets\n");
	if(setSecrets(c, 0) < 0){
		tlsError(c, EHandshakeFailure, "can't set secrets: %r");
		goto Err;
	}

	/* no CertificateVerify; skip to Finished */
	if(tlsSecFinished(c->sec, c->handhash, c->finished.verify, c->finished.n, 1) < 0){
		tlsError(c, EInternalError, "can't set finished: %r");
		goto Err;
	}
	if(!msgRecv(c, &m))
		goto Err;
	if(m.tag != HFinished) {
		tlsError(c, EUnexpectedMessage, "expected a finished");
		goto Err;
	}
	if(!finishedMatch(c, &m.u.finished)) {
		tlsError(c, EHandshakeFailure, "finished verification failed");
		goto Err;
	}
	msgClear(&m);

	/* change cipher spec */
	if(fprint(c->ctl, "changecipher") < 0){
		tlsError(c, EInternalError, "can't enable cipher: %r");
		goto Err;
	}

	if(tlsSecFinished(c->sec, c->handhash, c->finished.verify, c->finished.n, 0) < 0){
		tlsError(c, EInternalError, "can't set finished: %r");
		goto Err;
	}
	m.tag = HFinished;
	m.u.finished = c->finished;
	if(!msgSend(c, &m, AFlush))
		goto Err;
	if(trace)
		trace("tls finished\n");

	if(fprint(c->ctl, "opened") < 0)
		goto Err;
	return c;

Err:
	msgClear(&m);
	tlsConnectionFree(c);
	return nil;
}

static Bytes*
tlsSecDHEc(TlsSec *sec, Bytes *p, Bytes *g, Bytes *Ys)
{
	DHstate *dh = &sec->dh;
	mpint *G, *P, *Y, *K;
	Bytes *Yc;
	int n;

	if(p == nil || g == nil || Ys == nil)
		return nil;
	// reject dh primes that is susceptible to logjam
	if(p->len <= 1024/8)
		return nil;
	Yc = nil;
	P = bytestomp(p);
	G = bytestomp(g);
	Y = bytestomp(Ys);
	K = nil;

	if(dh_new(dh, P, nil, G) == nil)
		goto Out;
	n = (mpsignif(P)+7)/8;
	Yc = mptobytes(dh->y, n);
	K = dh_finish(dh, Y);	/* zeros dh */
	if(K == nil){
		freebytes(Yc);
		Yc = nil;
		goto Out;
	}
	setMasterSecret(sec, mptobytes(K, n));

Out:
	mpfree(K);
	mpfree(Y);
	mpfree(G);
	mpfree(P);

	return Yc;
}

static Bytes*
tlsSecECDHEc(TlsSec *sec, int curve, Bytes *Ys)
{
	ECdomain *dom = &sec->ec.dom;
	ECpriv *Q = &sec->ec.Q;
	ECpub *pub;
	ECpoint K;
	Namedcurve *nc;
	Bytes *Yc;
	Bytes *Z;
	int n;

	if(Ys == nil)
		return nil;

	if(curve == X25519){
		if(Ys->len != 32)
			return nil;
		Yc = newbytes(32);
		curve25519_dh_new(sec->X, Yc->data);
		Z = newbytes(32);
		if(!curve25519_dh_finish(sec->X, Ys->data, Z->data)){
			freebytes(Yc);
			freebytes(Z);
			return nil;
		}
		setMasterSecret(sec, Z);
	}else{
		for(nc = namedcurves; nc->tlsid != curve; nc++)
			if(nc == &namedcurves[nelem(namedcurves)])
				return nil;
		ecdominit(dom, nc->init);
		pub = ecdecodepub(dom, Ys->data, Ys->len);
		if(pub == nil)
			return nil;

		memset(Q, 0, sizeof(*Q));
		Q->a.x = mpnew(0);
		Q->a.y = mpnew(0);
		Q->d = mpnew(0);

		memset(&K, 0, sizeof(K));
		K.x = mpnew(0);
		K.y = mpnew(0);

		ecgen(dom, Q);
		ecmul(dom, pub, Q->d, &K);

		n = (mpsignif(dom->p)+7)/8;
		setMasterSecret(sec, mptobytes(K.x, n));
		Yc = newbytes(1 + 2*n);
		Yc->len = ecencodepub(dom, &Q->a, Yc->data, Yc->len);

		mpfree(K.x);
		mpfree(K.y);

		ecpubfree(pub);
	}
	return Yc;
}

static TlsConnection *
tlsClient2(int ctl, int hand,
	uchar *cert, int certlen,
	char *pskid, uchar *psk, int psklen,
	uchar *ext, int extlen,
	int (*trace)(char*fmt, ...))
{
	int creq, dhx, cipher;
	TlsConnection *c;
	Bytes *epm;
	Msg m;

	if(!initCiphers())
		return nil;

	epm = nil;
	memset(&m, 0, sizeof(m));
	c = emalloc(sizeof(TlsConnection));

	c->ctl = ctl;
	c->hand = hand;
	c->trace = trace;
	c->cert = nil;
	c->sendp = c->buf;

	c->version = ProtocolVersion;
	tlsSecInitc(c->sec, c->version);
	if(psklen > 0){
		c->sec->psk = psk;
		c->sec->psklen = psklen;
	}
	if(certlen > 0){
		/* client certificate */
		c->sec->rsapub = X509toRSApub(cert, certlen, nil, 0);
		if(c->sec->rsapub == nil){
			tlsError(c, EInternalError, "invalid X509/rsa certificate");
			goto Err;
		}
		c->sec->rpc = factotum_rsa_open(c->sec->rsapub);
		if(c->sec->rpc == nil){
			tlsError(c, EInternalError, "factotum_rsa_open: %r");
			goto Err;
		}
	}

	/* client hello */
	m.tag = HClientHello;
	m.u.clientHello.version = c->version;
	memmove(m.u.clientHello.random, c->sec->crandom, RandomSize);
	m.u.clientHello.sid = makebytes(nil, 0);
	m.u.clientHello.ciphers = makeciphers(psklen > 0);
	m.u.clientHello.compressors = makebytes(compressors,sizeof(compressors));
	m.u.clientHello.extensions = makebytes(ext, extlen);
	if(!msgSend(c, &m, AFlush))
		goto Err;

	/* server hello */
	if(!msgRecv(c, &m))
		goto Err;
	if(m.tag != HServerHello) {
		tlsError(c, EUnexpectedMessage, "expected a server hello");
		goto Err;
	}
	if(setVersion(c, m.u.serverHello.version) < 0) {
		tlsError(c, EIllegalParameter, "incompatible version: %r");
		goto Err;
	}
	tlsSecVers(c->sec, c->version);
	memmove(c->sec->srandom, m.u.serverHello.random, RandomSize);

	cipher = m.u.serverHello.cipher;
	if((psklen > 0) != isPSK(cipher) || !setAlgs(c, cipher)) {
		tlsError(c, EIllegalParameter, "invalid cipher suite");
		goto Err;
	}
	if(m.u.serverHello.compressor != CompressionNull) {
		tlsError(c, EIllegalParameter, "invalid compression");
		goto Err;
	}
	dhx = isDHE(cipher) || isECDHE(cipher);
	if(!msgRecv(c, &m))
		goto Err;
	if(m.tag == HCertificate){
		if(m.u.certificate.ncert < 1) {
			tlsError(c, EIllegalParameter, "runt certificate");
			goto Err;
		}
		c->cert = makebytes(m.u.certificate.certs[0]->data, m.u.certificate.certs[0]->len);
		if(!msgRecv(c, &m))
			goto Err;
	} else if(psklen == 0) {
		tlsError(c, EUnexpectedMessage, "expected a certificate");
		goto Err;
	}
	if(m.tag == HServerKeyExchange) {
		if(dhx){
			char *err = verifyDHparams(c->sec,
				m.u.serverKeyExchange.dh_parameters,
				c->cert,
				m.u.serverKeyExchange.dh_signature,
				c->version<TLS12Version ? 0x01 : m.u.serverKeyExchange.sigalg);
			if(err != nil){
				tlsError(c, EBadCertificate, "can't verify DH parameters: %s", err);
				goto Err;
			}
			if(isECDHE(cipher))
				epm = tlsSecECDHEc(c->sec,
					m.u.serverKeyExchange.curve,
					m.u.serverKeyExchange.dh_Ys);
			else
				epm = tlsSecDHEc(c->sec,
					m.u.serverKeyExchange.dh_p, 
					m.u.serverKeyExchange.dh_g,
					m.u.serverKeyExchange.dh_Ys);
			if(epm == nil){
				tlsError(c, EHandshakeFailure, "bad DH parameters");
				goto Err;
			}
		} else if(psklen == 0){
			tlsError(c, EUnexpectedMessage, "got an server key exchange");
			goto Err;
		}
		if(!msgRecv(c, &m))
			goto Err;
	} else if(dhx){
		tlsError(c, EUnexpectedMessage, "expected server key exchange");
		goto Err;
	}

	/* certificate request (optional) */
	creq = 0;
	if(m.tag == HCertificateRequest) {
		creq = 1;
		if(!msgRecv(c, &m))
			goto Err;
	}

	if(m.tag != HServerHelloDone) {
		tlsError(c, EUnexpectedMessage, "expected a server hello done");
		goto Err;
	}
	msgClear(&m);

	if(!dhx){
		if(c->cert != nil){
			epm = tlsSecRSAc(c->sec, c->cert->data, c->cert->len);
			if(epm == nil){
				tlsError(c, EBadCertificate, "bad certificate: %r");
				goto Err;
			}
		} else if(psklen > 0){
			setMasterSecret(c->sec, newbytes(psklen));
		} else {
			tlsError(c, EInternalError, "no psk or certificate");
			goto Err;
		}
	}

	if(trace)
		trace("tls secrets\n");
	if(setSecrets(c, 1) < 0){
		tlsError(c, EHandshakeFailure, "can't set secrets: %r");
		goto Err;
	}

	if(creq) {
		m.tag = HCertificate;
		if(certlen > 0){
			m.u.certificate.ncert = 1;
			m.u.certificate.certs = emalloc(m.u.certificate.ncert * sizeof(Bytes*));
			m.u.certificate.certs[0] = makebytes(cert, certlen);
		}		
		if(!msgSend(c, &m, AFlush))
			goto Err;
	}

	/* client key exchange */
	m.tag = HClientKeyExchange;
	if(psklen > 0){
		if(pskid == nil)
			pskid = "";
		m.u.clientKeyExchange.pskid = makebytes((uchar*)pskid, strlen(pskid));
	}
	m.u.clientKeyExchange.key = epm;
	epm = nil;
	 
	if(!msgSend(c, &m, AFlush))
		goto Err;

	/* certificate verify */
	if(creq && certlen > 0) {
		HandshakeHash hsave;
		uchar digest[MAXdlen];
		int digestlen;

		/* save the state for the Finish message */
		hsave = c->handhash;
		if(c->version < TLS12Version){
			md5(nil, 0, digest, &c->handhash.md5);
			sha1(nil, 0, digest+MD5dlen, &c->handhash.sha1);
			digestlen = MD5dlen+SHA1dlen;
		} else {
			m.u.certificateVerify.sigalg = 0x0401;	/* RSA SHA256 */
			sha2_256(nil, 0, digest, &c->handhash.sha2_256);
			digestlen = SHA2_256dlen;
		}
		c->handhash = hsave;

		if((m.u.certificateVerify.signature = pkcs1_sign(c->sec, digest, digestlen,
			m.u.certificateVerify.sigalg)) == nil){
			tlsError(c, EHandshakeFailure, "pkcs1_sign: %r");
			goto Err;
		}

		m.tag = HCertificateVerify;
		if(!msgSend(c, &m, AFlush))
			goto Err;
	} 

	/* change cipher spec */
	if(fprint(c->ctl, "changecipher") < 0){
		tlsError(c, EInternalError, "can't enable cipher: %r");
		goto Err;
	}

	// Cipherchange must occur immediately before Finished to avoid
	// potential hole;  see section 4.3 of Wagner Schneier 1996.
	if(tlsSecFinished(c->sec, c->handhash, c->finished.verify, c->finished.n, 1) < 0){
		tlsError(c, EInternalError, "can't set finished 1: %r");
		goto Err;
	}
	m.tag = HFinished;
	m.u.finished = c->finished;
	if(!msgSend(c, &m, AFlush)) {
		tlsError(c, EInternalError, "can't flush after client Finished: %r");
		goto Err;
	}

	if(tlsSecFinished(c->sec, c->handhash, c->finished.verify, c->finished.n, 0) < 0){
		tlsError(c, EInternalError, "can't set finished 0: %r");
		goto Err;
	}
	if(!msgRecv(c, &m)) {
		tlsError(c, EInternalError, "can't read server Finished: %r");
		goto Err;
	}
	if(m.tag != HFinished) {
		tlsError(c, EUnexpectedMessage, "expected a Finished msg from server");
		goto Err;
	}

	if(!finishedMatch(c, &m.u.finished)) {
		tlsError(c, EHandshakeFailure, "finished verification failed");
		goto Err;
	}
	msgClear(&m);

	if(fprint(c->ctl, "opened") < 0){
		if(trace)
			trace("unable to do final open: %r\n");
		goto Err;
	}
	return c;

Err:
	free(epm);
	msgClear(&m);
	tlsConnectionFree(c);
	return nil;
}


//================= message functions ========================

static void
msgHash(TlsConnection *c, uchar *p, int n)
{
	md5(p, n, 0, &c->handhash.md5);
	sha1(p, n, 0, &c->handhash.sha1);
	if(c->version >= TLS12Version)
		sha2_256(p, n, 0, &c->handhash.sha2_256);
}

static int
msgSend(TlsConnection *c, Msg *m, int act)
{
	uchar *p, *e; // sendp = start of new message;  p = write pointer; e = end pointer
	int n, i;

	p = c->sendp;
	e = &c->buf[sizeof(c->buf)];
	if(c->trace)
		c->trace("send %s", msgPrint((char*)p, e - p, m));

	p[0] = m->tag;	// header - fill in size later
	p += 4;

	switch(m->tag) {
	default:
		tlsError(c, EInternalError, "can't encode a %d", m->tag);
		goto Err;
	case HClientHello:
		if(p+2+RandomSize > e)
			goto Overflow;
		put16(p, m->u.clientHello.version), p += 2;
		memmove(p, m->u.clientHello.random, RandomSize);
		p += RandomSize;

		if(p+1+(n = m->u.clientHello.sid->len) > e)
			goto Overflow;
		*p++ = n;
		memmove(p, m->u.clientHello.sid->data, n);
		p += n;

		if(p+2+(n = m->u.clientHello.ciphers->len) > e)
			goto Overflow;
		put16(p, n*2), p += 2;
		for(i=0; i<n; i++)
			put16(p, m->u.clientHello.ciphers->data[i]), p += 2;

		if(p+1+(n = m->u.clientHello.compressors->len) > e)
			goto Overflow;
		*p++ = n;
		memmove(p, m->u.clientHello.compressors->data, n);
		p += n;

		if(m->u.clientHello.extensions == nil
		|| (n = m->u.clientHello.extensions->len) == 0)
			break;
		if(p+2+n > e)
			goto Overflow;
		put16(p, n), p += 2;
		memmove(p, m->u.clientHello.extensions->data, n);
		p += n;
		break;
	case HServerHello:
		if(p+2+RandomSize > e)
			goto Overflow;
		put16(p, m->u.serverHello.version), p += 2;
		memmove(p, m->u.serverHello.random, RandomSize);
		p += RandomSize;

		if(p+1+(n = m->u.serverHello.sid->len) > e)
			goto Overflow;
		*p++ = n;
		memmove(p, m->u.serverHello.sid->data, n);
		p += n;

		if(p+2+1 > e)
			goto Overflow;
		put16(p, m->u.serverHello.cipher), p += 2;
		*p++ = m->u.serverHello.compressor;

		if(m->u.serverHello.extensions == nil
		|| (n = m->u.serverHello.extensions->len) == 0)
			break;
		if(p+2+n > e)
			goto Overflow;
		put16(p, n), p += 2;
		memmove(p, m->u.serverHello.extensions->data, n);
		p += n;
		break;
	case HServerHelloDone:
		break;
	case HCertificate:
		n = 0;
		for(i = 0; i < m->u.certificate.ncert; i++)
			n += 3 + m->u.certificate.certs[i]->len;
		if(p+3+n > e)
			goto Overflow;
		put24(p, n), p += 3;
		for(i = 0; i < m->u.certificate.ncert; i++){
			n = m->u.certificate.certs[i]->len;
			put24(p, n), p += 3;
			memmove(p, m->u.certificate.certs[i]->data, n);
			p += n;
		}
		break;
	case HCertificateVerify:
		if(p+2+2+(n = m->u.certificateVerify.signature->len) > e)
			goto Overflow;
		if(m->u.certificateVerify.sigalg != 0)
			put16(p, m->u.certificateVerify.sigalg), p += 2;
		put16(p, n), p += 2;
		memmove(p, m->u.certificateVerify.signature->data, n);
		p += n;
		break;
	case HServerKeyExchange:
		if(m->u.serverKeyExchange.pskid != nil){
			if(p+2+(n = m->u.serverKeyExchange.pskid->len) > e)
				goto Overflow;
			put16(p, n), p += 2;
			memmove(p, m->u.serverKeyExchange.pskid->data, n);
			p += n;
		}
		if(m->u.serverKeyExchange.dh_parameters == nil)
			break;
		if(p+(n = m->u.serverKeyExchange.dh_parameters->len) > e)
			goto Overflow;
		memmove(p, m->u.serverKeyExchange.dh_parameters->data, n);
		p += n;
		if(m->u.serverKeyExchange.dh_signature == nil)
			break;
		if(p+2+2+(n = m->u.serverKeyExchange.dh_signature->len) > e)
			goto Overflow;
		if(c->version >= TLS12Version)
			put16(p, m->u.serverKeyExchange.sigalg), p += 2;
		put16(p, n), p += 2;
		memmove(p, m->u.serverKeyExchange.dh_signature->data, n);
		p += n;
		break;
	case HClientKeyExchange:
		if(m->u.clientKeyExchange.pskid != nil){
			if(p+2+(n = m->u.clientKeyExchange.pskid->len) > e)
				goto Overflow;
			put16(p, n), p += 2;
			memmove(p, m->u.clientKeyExchange.pskid->data, n);
			p += n;
		}
		if(m->u.clientKeyExchange.key == nil)
			break;
		if(p+2+(n = m->u.clientKeyExchange.key->len) > e)
			goto Overflow;
		if(isECDHE(c->cipher))
			*p++ = n;
		else if(isDHE(c->cipher) || c->version != SSL3Version)
			put16(p, n), p += 2;
		memmove(p, m->u.clientKeyExchange.key->data, n);
		p += n;
		break;
	case HFinished:
		if(p+m->u.finished.n > e)
			goto Overflow;
		memmove(p, m->u.finished.verify, m->u.finished.n);
		p += m->u.finished.n;
		break;
	}

	// go back and fill in size
	n = p - c->sendp;
	put24(c->sendp+1, n-4);

	// remember hash of Handshake messages
	if(m->tag != HHelloRequest)
		msgHash(c, c->sendp, n);

	c->sendp = p;
	if(act == AFlush){
		c->sendp = c->buf;
		if(write(c->hand, c->buf, p - c->buf) < 0){
			fprint(2, "write error: %r\n");
			goto Err;
		}
	}
	msgClear(m);
	return 1;
Overflow:
	tlsError(c, EInternalError, "not enougth send buffer for message (%d)", m->tag);
Err:
	msgClear(m);
	return 0;
}

static uchar*
tlsReadN(TlsConnection *c, int n)
{
	uchar *p, *w, *e;

	e = &c->buf[sizeof(c->buf)];
	p = e - n;
	if(n > (int)sizeof(c->buf) || p < c->sendp){
		tlsError(c, EDecodeError, "handshake message too long %d", n);
		return nil;
	}
	for(w = p; w < e; w += n)
		if((n = read(c->hand, w, e - w)) <= 0)
			return nil;
	return p;
}

static int
msgRecv(TlsConnection *c, Msg *m)
{
	uchar *p, *s;
	int type, n, nn, i;

	msgClear(m);
	for(;;) {
		p = tlsReadN(c, 4);
		if(p == nil)
			return 0;
		type = p[0];
		n = get24(p+1);

		if(type != HHelloRequest)
			break;
		if(n != 0) {
			tlsError(c, EDecodeError, "invalid hello request during handshake");
			return 0;
		}
	}

	if(type == HSSL2ClientHello){
		/* Cope with an SSL3 ClientHello expressed in SSL2 record format.
			This is sent by some clients that we must interoperate
			with, such as Java's JSSE and Microsoft's Internet Explorer. */
		int nsid, nrandom, nciph;

		p = tlsReadN(c, n);
		if(p == nil)
			return 0;
		msgHash(c, p, n);
		m->tag = HClientHello;
		if(n < 22)
			goto Short;
		m->u.clientHello.version = get16(p+1);
		p += 3;
		n -= 3;
		nn = get16(p); /* cipher_spec_len */
		nsid = get16(p + 2);
		nrandom = get16(p + 4);
		p += 6;
		n -= 6;
		if(nsid != 0 	/* no sid's, since shouldn't restart using ssl2 header */
		|| nrandom < 16 || nn % 3 || n - nrandom < nn)
			goto Err;
		/* ignore ssl2 ciphers and look for {0x00, ssl3 cipher} */
		nciph = 0;
		for(i = 0; i < nn; i += 3)
			if(p[i] == 0)
				nciph++;
		m->u.clientHello.ciphers = newints(nciph);
		nciph = 0;
		for(i = 0; i < nn; i += 3)
			if(p[i] == 0)
				m->u.clientHello.ciphers->data[nciph++] = get16(&p[i + 1]);
		p += nn;
		m->u.clientHello.sid = makebytes(nil, 0);
		if(nrandom > RandomSize)
			nrandom = RandomSize;
		memset(m->u.clientHello.random, 0, RandomSize - nrandom);
		memmove(&m->u.clientHello.random[RandomSize - nrandom], p, nrandom);
		m->u.clientHello.compressors = newbytes(1);
		m->u.clientHello.compressors->data[0] = CompressionNull;
		goto Ok;
	}
	msgHash(c, p, 4);

	p = tlsReadN(c, n);
	if(p == nil)
		return 0;

	msgHash(c, p, n);

	m->tag = type;

	switch(type) {
	default:
		tlsError(c, EUnexpectedMessage, "can't decode a %d", type);
		goto Err;
	case HClientHello:
		if(n < 2)
			goto Short;
		m->u.clientHello.version = get16(p);
		p += 2, n -= 2;

		if(n < RandomSize)
			goto Short;
		memmove(m->u.clientHello.random, p, RandomSize);
		p += RandomSize, n -= RandomSize;
		if(n < 1 || n < p[0]+1)
			goto Short;
		m->u.clientHello.sid = makebytes(p+1, p[0]);
		p += m->u.clientHello.sid->len+1;
		n -= m->u.clientHello.sid->len+1;

		if(n < 2)
			goto Short;
		nn = get16(p);
		p += 2, n -= 2;

		if(nn % 2 || n < nn || nn < 2)
			goto Short;
		m->u.clientHello.ciphers = newints(nn >> 1);
		for(i = 0; i < nn; i += 2)
			m->u.clientHello.ciphers->data[i >> 1] = get16(&p[i]);
		p += nn, n -= nn;

		if(n < 1 || n < p[0]+1 || p[0] == 0)
			goto Short;
		nn = p[0];
		m->u.clientHello.compressors = makebytes(p+1, nn);
		p += nn + 1, n -= nn + 1;

		if(n < 2)
			break;
		nn = get16(p);
		if(nn > n-2)
			goto Short;
		m->u.clientHello.extensions = makebytes(p+2, nn);
		n -= nn + 2;
		break;
	case HServerHello:
		if(n < 2)
			goto Short;
		m->u.serverHello.version = get16(p);
		p += 2, n -= 2;

		if(n < RandomSize)
			goto Short;
		memmove(m->u.serverHello.random, p, RandomSize);
		p += RandomSize, n -= RandomSize;

		if(n < 1 || n < p[0]+1)
			goto Short;
		m->u.serverHello.sid = makebytes(p+1, p[0]);
		p += m->u.serverHello.sid->len+1;
		n -= m->u.serverHello.sid->len+1;

		if(n < 3)
			goto Short;
		m->u.serverHello.cipher = get16(p);
		m->u.serverHello.compressor = p[2];
		p += 3, n -= 3;

		if(n < 2)
			break;
		nn = get16(p);
		if(nn > n-2)
			goto Short;
		m->u.serverHello.extensions = makebytes(p+2, nn);
		n -= nn + 2;
		break;
	case HCertificate:
		if(n < 3)
			goto Short;
		nn = get24(p);
		p += 3, n -= 3;
		if(nn == 0 && n > 0)
			goto Short;
		/* certs */
		i = 0;
		while(n > 0) {
			if(n < 3)
				goto Short;
			nn = get24(p);
			p += 3, n -= 3;
			if(nn > n)
				goto Short;
			m->u.certificate.ncert = i+1;
			m->u.certificate.certs = erealloc(m->u.certificate.certs, (i+1)*sizeof(Bytes*));
			m->u.certificate.certs[i] = makebytes(p, nn);
			p += nn, n -= nn;
			i++;
		}
		break;
	case HCertificateRequest:
		if(n < 1)
			goto Short;
		nn = p[0];
		p++, n--;
		if(nn > n)
			goto Short;
		m->u.certificateRequest.types = makebytes(p, nn);
		p += nn, n -= nn;
		if(c->version >= TLS12Version){
			if(n < 2)
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			if(nn % 2)
				goto Short;
			m->u.certificateRequest.sigalgs = newints(nn>>1);
			for(i = 0; i < nn; i += 2)
				m->u.certificateRequest.sigalgs->data[i >> 1] = get16(&p[i]);
			p += nn, n -= nn;

		}
		if(n < 2)
			goto Short;
		nn = get16(p);
		p += 2, n -= 2;
		/* nn == 0 can happen; yahoo's servers do it */
		if(nn != n)
			goto Short;
		/* cas */
		i = 0;
		while(n > 0) {
			if(n < 2)
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			if(nn < 1 || nn > n)
				goto Short;
			m->u.certificateRequest.nca = i+1;
			m->u.certificateRequest.cas = erealloc(
				m->u.certificateRequest.cas, (i+1)*sizeof(Bytes*));
			m->u.certificateRequest.cas[i] = makebytes(p, nn);
			p += nn, n -= nn;
			i++;
		}
		break;
	case HServerHelloDone:
		break;
	case HServerKeyExchange:
		if(isPSK(c->cipher)){
			if(n < 2)
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			if(nn > n)
				goto Short;
			m->u.serverKeyExchange.pskid = makebytes(p, nn);
			p += nn, n -= nn;
			if(n == 0)
				break;
		}
		if(n < 2)
			goto Short;
		s = p;
		if(isECDHE(c->cipher)){
			nn = *p;
			p++, n--;
			if(nn != 3 || nn > n) /* not a named curve */
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			m->u.serverKeyExchange.curve = nn;

			nn = *p++, n--;
			if(nn < 1 || nn > n)
				goto Short;
			m->u.serverKeyExchange.dh_Ys = makebytes(p, nn);
			p += nn, n -= nn;
		}else if(isDHE(c->cipher)){
			nn = get16(p);
			p += 2, n -= 2;
			if(nn < 1 || nn > n)
				goto Short;
			m->u.serverKeyExchange.dh_p = makebytes(p, nn);
			p += nn, n -= nn;
	
			if(n < 2)
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			if(nn < 1 || nn > n)
				goto Short;
			m->u.serverKeyExchange.dh_g = makebytes(p, nn);
			p += nn, n -= nn;
	
			if(n < 2)
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			if(nn < 1 || nn > n)
				goto Short;
			m->u.serverKeyExchange.dh_Ys = makebytes(p, nn);
			p += nn, n -= nn;
		} else {
			/* should not happen */
			goto Short;
		}
		m->u.serverKeyExchange.dh_parameters = makebytes(s, p - s);
		if(n >= 2){
			m->u.serverKeyExchange.sigalg = 0;
			if(c->version >= TLS12Version){
				m->u.serverKeyExchange.sigalg = get16(p);
				p += 2, n -= 2;
				if(n < 2)
					goto Short;
			}
			nn = get16(p);
			p += 2, n -= 2;
			if(nn > 0 && nn <= n){
				m->u.serverKeyExchange.dh_signature = makebytes(p, nn);
				n -= nn;
			}
		}
		break;		
	case HClientKeyExchange:
		if(isPSK(c->cipher)){
			if(n < 2)
				goto Short;
			nn = get16(p);
			p += 2, n -= 2;
			if(nn > n)
				goto Short;
			m->u.clientKeyExchange.pskid = makebytes(p, nn);
			p += nn, n -= nn;
			if(n == 0)
				break;
		}
		if(n < 2)
			goto Short;
		if(isECDHE(c->cipher))
			nn = *p++, n--;
		else if(isDHE(c->cipher) || c->version != SSL3Version)
			nn = get16(p), p += 2, n -= 2;
		else
			nn = n;
		if(n < nn)
			goto Short;
		m->u.clientKeyExchange.key = makebytes(p, nn);
		n -= nn;
		break;
	case HFinished:
		m->u.finished.n = c->finished.n;
		if(n < m->u.finished.n)
			goto Short;
		memmove(m->u.finished.verify, p, m->u.finished.n);
		n -= m->u.finished.n;
		break;
	}

	if(n != 0 && type != HClientHello && type != HServerHello)
		goto Short;
Ok:
	if(c->trace)
		c->trace("recv %s", msgPrint((char*)c->sendp, &c->buf[sizeof(c->buf)] - c->sendp, m));
	return 1;
Short:
	tlsError(c, EDecodeError, "handshake message (%d) has invalid length", type);
Err:
	msgClear(m);
	return 0;
}

static void
msgClear(Msg *m)
{
	int i;

	switch(m->tag) {
	case HHelloRequest:
		break;
	case HClientHello:
		freebytes(m->u.clientHello.sid);
		freeints(m->u.clientHello.ciphers);
		freebytes(m->u.clientHello.compressors);
		freebytes(m->u.clientHello.extensions);
		break;
	case HServerHello:
		freebytes(m->u.serverHello.sid);
		freebytes(m->u.serverHello.extensions);
		break;
	case HCertificate:
		for(i=0; i<m->u.certificate.ncert; i++)
			freebytes(m->u.certificate.certs[i]);
		free(m->u.certificate.certs);
		break;
	case HCertificateRequest:
		freebytes(m->u.certificateRequest.types);
		freeints(m->u.certificateRequest.sigalgs);
		for(i=0; i<m->u.certificateRequest.nca; i++)
			freebytes(m->u.certificateRequest.cas[i]);
		free(m->u.certificateRequest.cas);
		break;
	case HCertificateVerify:
		freebytes(m->u.certificateVerify.signature);
		break;
	case HServerHelloDone:
		break;
	case HServerKeyExchange:
		freebytes(m->u.serverKeyExchange.pskid);
		freebytes(m->u.serverKeyExchange.dh_p);
		freebytes(m->u.serverKeyExchange.dh_g);
		freebytes(m->u.serverKeyExchange.dh_Ys);
		freebytes(m->u.serverKeyExchange.dh_parameters);
		freebytes(m->u.serverKeyExchange.dh_signature);
		break;
	case HClientKeyExchange:
		freebytes(m->u.clientKeyExchange.pskid);
		freebytes(m->u.clientKeyExchange.key);
		break;
	case HFinished:
		break;
	}
	memset(m, 0, sizeof(Msg));
}

static char *
bytesPrint(char *bs, char *be, char *s0, Bytes *b, char *s1)
{
	int i;

	if(s0)
		bs = seprint(bs, be, "%s", s0);
	if(b == nil)
		bs = seprint(bs, be, "nil");
	else {
		bs = seprint(bs, be, "<%d> [ ", b->len);
		for(i=0; i<b->len; i++)
			bs = seprint(bs, be, "%.2x ", b->data[i]);
		bs = seprint(bs, be, "]");
	}
	if(s1)
		bs = seprint(bs, be, "%s", s1);
	return bs;
}

static char *
intsPrint(char *bs, char *be, char *s0, Ints *b, char *s1)
{
	int i;

	if(s0)
		bs = seprint(bs, be, "%s", s0);
	if(b == nil)
		bs = seprint(bs, be, "nil");
	else {
		bs = seprint(bs, be, "[ ");
		for(i=0; i<b->len; i++)
			bs = seprint(bs, be, "%x ", b->data[i]);
		bs = seprint(bs, be, "]");
	}
	if(s1)
		bs = seprint(bs, be, "%s", s1);
	return bs;
}

static char*
msgPrint(char *buf, int n, Msg *m)
{
	int i;
	char *bs = buf, *be = buf+n;

	switch(m->tag) {
	default:
		bs = seprint(bs, be, "unknown %d\n", m->tag);
		break;
	case HClientHello:
		bs = seprint(bs, be, "ClientHello\n");
		bs = seprint(bs, be, "\tversion: %.4x\n", m->u.clientHello.version);
		bs = seprint(bs, be, "\trandom: ");
		for(i=0; i<RandomSize; i++)
			bs = seprint(bs, be, "%.2x", m->u.clientHello.random[i]);
		bs = seprint(bs, be, "\n");
		bs = bytesPrint(bs, be, "\tsid: ", m->u.clientHello.sid, "\n");
		bs = intsPrint(bs, be, "\tciphers: ", m->u.clientHello.ciphers, "\n");
		bs = bytesPrint(bs, be, "\tcompressors: ", m->u.clientHello.compressors, "\n");
		if(m->u.clientHello.extensions != nil)
			bs = bytesPrint(bs, be, "\textensions: ", m->u.clientHello.extensions, "\n");
		break;
	case HServerHello:
		bs = seprint(bs, be, "ServerHello\n");
		bs = seprint(bs, be, "\tversion: %.4x\n", m->u.serverHello.version);
		bs = seprint(bs, be, "\trandom: ");
		for(i=0; i<RandomSize; i++)
			bs = seprint(bs, be, "%.2x", m->u.serverHello.random[i]);
		bs = seprint(bs, be, "\n");
		bs = bytesPrint(bs, be, "\tsid: ", m->u.serverHello.sid, "\n");
		bs = seprint(bs, be, "\tcipher: %.4x\n", m->u.serverHello.cipher);
		bs = seprint(bs, be, "\tcompressor: %.2x\n", m->u.serverHello.compressor);
		if(m->u.serverHello.extensions != nil)
			bs = bytesPrint(bs, be, "\textensions: ", m->u.serverHello.extensions, "\n");
		break;
	case HCertificate:
		bs = seprint(bs, be, "Certificate\n");
		for(i=0; i<m->u.certificate.ncert; i++)
			bs = bytesPrint(bs, be, "\t", m->u.certificate.certs[i], "\n");
		break;
	case HCertificateRequest:
		bs = seprint(bs, be, "CertificateRequest\n");
		bs = bytesPrint(bs, be, "\ttypes: ", m->u.certificateRequest.types, "\n");
		if(m->u.certificateRequest.sigalgs != nil)
			bs = intsPrint(bs, be, "\tsigalgs: ", m->u.certificateRequest.sigalgs, "\n");
		bs = seprint(bs, be, "\tcertificateauthorities\n");
		for(i=0; i<m->u.certificateRequest.nca; i++)
			bs = bytesPrint(bs, be, "\t\t", m->u.certificateRequest.cas[i], "\n");
		break;
	case HCertificateVerify:
		bs = seprint(bs, be, "HCertificateVerify\n");
		if(m->u.certificateVerify.sigalg != 0)
			bs = seprint(bs, be, "\tsigalg: %.4x\n", m->u.certificateVerify.sigalg);
		bs = bytesPrint(bs, be, "\tsignature: ", m->u.certificateVerify.signature,"\n");
		break;	
	case HServerHelloDone:
		bs = seprint(bs, be, "ServerHelloDone\n");
		break;
	case HServerKeyExchange:
		bs = seprint(bs, be, "HServerKeyExchange\n");
		if(m->u.serverKeyExchange.pskid != nil)
			bs = bytesPrint(bs, be, "\tpskid: ", m->u.serverKeyExchange.pskid, "\n");
		if(m->u.serverKeyExchange.dh_parameters == nil)
			break;
		if(m->u.serverKeyExchange.curve != 0){
			bs = seprint(bs, be, "\tcurve: %.4x\n", m->u.serverKeyExchange.curve);
		} else {
			bs = bytesPrint(bs, be, "\tdh_p: ", m->u.serverKeyExchange.dh_p, "\n");
			bs = bytesPrint(bs, be, "\tdh_g: ", m->u.serverKeyExchange.dh_g, "\n");
		}
		bs = bytesPrint(bs, be, "\tdh_Ys: ", m->u.serverKeyExchange.dh_Ys, "\n");
		if(m->u.serverKeyExchange.sigalg != 0)
			bs = seprint(bs, be, "\tsigalg: %.4x\n", m->u.serverKeyExchange.sigalg);
		bs = bytesPrint(bs, be, "\tdh_parameters: ", m->u.serverKeyExchange.dh_parameters, "\n");
		bs = bytesPrint(bs, be, "\tdh_signature: ", m->u.serverKeyExchange.dh_signature, "\n");
		break;
	case HClientKeyExchange:
		bs = seprint(bs, be, "HClientKeyExchange\n");
		if(m->u.clientKeyExchange.pskid != nil)
			bs = bytesPrint(bs, be, "\tpskid: ", m->u.clientKeyExchange.pskid, "\n");
		if(m->u.clientKeyExchange.key != nil)
			bs = bytesPrint(bs, be, "\tkey: ", m->u.clientKeyExchange.key, "\n");
		break;
	case HFinished:
		bs = seprint(bs, be, "HFinished\n");
		for(i=0; i<m->u.finished.n; i++)
			bs = seprint(bs, be, "%.2x", m->u.finished.verify[i]);
		bs = seprint(bs, be, "\n");
		break;
	}
	USED(bs);
	return buf;
}

static void
tlsError(TlsConnection *c, int err, char *fmt, ...)
{
	char msg[512];
	va_list arg;

	va_start(arg, fmt);
	vseprint(msg, msg+sizeof(msg), fmt, arg);
	va_end(arg);
	if(c->trace)
		c->trace("tlsError: %s\n", msg);
	if(c->erred)
		fprint(2, "double error: %r, %s", msg);
	else
		errstr(msg, sizeof(msg));
	c->erred = 1;
	fprint(c->ctl, "alert %d", err);
}

// commit to specific version number
static int
setVersion(TlsConnection *c, int version)
{
	if(version > MaxProtoVersion || version < MinProtoVersion)
		return -1;
	if(version > c->version)
		version = c->version;
	if(version == SSL3Version) {
		c->version = version;
		c->finished.n = SSL3FinishedLen;
	}else {
		c->version = version;
		c->finished.n = TLSFinishedLen;
	}
	return fprint(c->ctl, "version 0x%x", version);
}

// confirm that received Finished message matches the expected value
static int
finishedMatch(TlsConnection *c, Finished *f)
{
	return tsmemcmp(f->verify, c->finished.verify, f->n) == 0;
}

// free memory associated with TlsConnection struct
//		(but don't close the TLS channel itself)
static void
tlsConnectionFree(TlsConnection *c)
{
	if(c == nil)
		return;

	dh_finish(&c->sec->dh, nil);

	mpfree(c->sec->ec.Q.a.x);
	mpfree(c->sec->ec.Q.a.y);
	mpfree(c->sec->ec.Q.d);
	ecdomfree(&c->sec->ec.dom);

	factotum_rsa_close(c->sec->rpc);
	rsapubfree(c->sec->rsapub);
	freebytes(c->cert);

	memset(c, 0, sizeof(*c));
	free(c);
}


//================= cipher choices ========================

static int
isDHE(int tlsid)
{
	switch(tlsid){
	case TLS_DHE_RSA_WITH_AES_128_GCM_SHA256:
	case TLS_DHE_RSA_WITH_AES_128_CBC_SHA256:
 	case TLS_DHE_RSA_WITH_AES_128_CBC_SHA:
 	case TLS_DHE_RSA_WITH_AES_256_CBC_SHA:
 	case TLS_DHE_RSA_WITH_3DES_EDE_CBC_SHA:
	case TLS_DHE_RSA_WITH_CHACHA20_POLY1305:
	case GOOGLE_DHE_RSA_WITH_CHACHA20_POLY1305:
		return 1;
	}
	return 0;
}

static int
isECDHE(int tlsid)
{
	switch(tlsid){
	case TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
	case TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305:

	case GOOGLE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
	case GOOGLE_ECDHE_RSA_WITH_CHACHA20_POLY1305:

	case TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
	case TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:

	case TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256:
	case TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256:
	case TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA:
	case TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA:
		return 1;
	}
	return 0;
}

static int
isPSK(int tlsid)
{
	switch(tlsid){
	case TLS_PSK_WITH_CHACHA20_POLY1305:
	case TLS_PSK_WITH_AES_128_CBC_SHA256:
	case TLS_PSK_WITH_AES_128_CBC_SHA:
		return 1;
	}
	return 0;
}

static int
isECDSA(int tlsid)
{
	switch(tlsid){
	case TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
	case GOOGLE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
	case TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
	case TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256:
		return 1;
	}
	return 0;
}

static int
setAlgs(TlsConnection *c, int a)
{
	uint i;

	for(i = 0; i < nelem(cipherAlgs); i++){
		if(cipherAlgs[i].tlsid == a){
			c->cipher = a;
			c->enc = cipherAlgs[i].enc;
			c->digest = cipherAlgs[i].digest;
			c->nsecret = cipherAlgs[i].nsecret;
			if(c->nsecret > MaxKeyData)
				return 0;
			return 1;
		}
	}
	return 0;
}

static int
okCipher(Ints *cv, int ispsk, int canec)
{
	uint i, c;

	for(i = 0; i < nelem(cipherAlgs); i++) {
		c = cipherAlgs[i].tlsid;
		if(!cipherAlgs[i].ok || isECDSA(c) || isDHE(c))
			continue;
		if(isPSK(c) != ispsk)
			continue;
		if(isECDHE(c) && !canec)
			continue;
		if(lookupid(cv, c) >= 0)
			return c;
	}
	return -1;
}

static int
okCompression(Bytes *cv)
{
	uint i, c;

	for(i = 0; i < nelem(compressors); i++) {
		c = compressors[i];
		if(memchr(cv->data, c, cv->len) != nil)
			return c;
	}
	return -1;
}

static Lock	ciphLock;
static int	nciphers;

static int
initCiphers(void)
{
	enum {MaxAlgF = 1024, MaxAlgs = 10};
	char s[MaxAlgF], *flds[MaxAlgs];
	uint i;
	int j, n, ok;

	lock(&ciphLock);
	if(nciphers){
		unlock(&ciphLock);
		return nciphers;
	}
	j = open("#a/tls/encalgs", OREAD|OCEXEC);
	if(j < 0){
		werrstr("can't open #a/tls/encalgs: %r");
		goto out;
	}
	n = read(j, s, MaxAlgF-1);
	close(j);
	if(n <= 0){
		werrstr("nothing in #a/tls/encalgs: %r");
		goto out;
	}
	s[n] = 0;
	n = getfields(s, flds, MaxAlgs, 1, " \t\r\n");
	for(i = 0; i < nelem(cipherAlgs); i++){
		ok = 0;
		for(j = 0; j < n; j++){
			if(strcmp(cipherAlgs[i].enc, flds[j]) == 0){
				ok = 1;
				break;
			}
		}
		cipherAlgs[i].ok = ok;
	}

	j = open("#a/tls/hashalgs", OREAD|OCEXEC);
	if(j < 0){
		werrstr("can't open #a/tls/hashalgs: %r");
		goto out;
	}
	n = read(j, s, MaxAlgF-1);
	close(j);
	if(n <= 0){
		werrstr("nothing in #a/tls/hashalgs: %r");
		goto out;
	}
	s[n] = 0;
	n = getfields(s, flds, MaxAlgs, 1, " \t\r\n");
	for(i = 0; i < nelem(cipherAlgs); i++){
		ok = 0;
		for(j = 0; j < n; j++){
			if(strcmp(cipherAlgs[i].digest, flds[j]) == 0){
				ok = 1;
				break;
			}
		}
		cipherAlgs[i].ok &= ok;
		if(cipherAlgs[i].ok)
			nciphers++;
	}
out:
	unlock(&ciphLock);
	return nciphers;
}

static Ints*
makeciphers(int ispsk)
{
	Ints *is;
	uint i, j;

	is = newints(nciphers);
	j = 0;
	for(i = 0; i < nelem(cipherAlgs); i++)
		if(cipherAlgs[i].ok && isPSK(cipherAlgs[i].tlsid) == ispsk)
			is->data[j++] = cipherAlgs[i].tlsid;
	is->len = j;
	return is;
}


//================= security functions ========================

// given a public key, set up connection to factotum
// for using corresponding private key
static AuthRpc*
factotum_rsa_open(RSApub *rsapub)
{
	int afd;
	char *s;
	mpint *n;
	AuthRpc *rpc;

	// start talking to factotum
	if((afd = open("/mnt/factotum/rpc", ORDWR|OCEXEC)) < 0)
		return nil;
	if((rpc = auth_allocrpc(afd)) == nil){
		close(afd);
		return nil;
	}
	s = "proto=rsa service=tls role=client";
	if(auth_rpc(rpc, "start", s, strlen(s)) == ARok){
		// roll factotum keyring around to match public key
		n = mpnew(0);
		while(auth_rpc(rpc, "read", nil, 0) == ARok){
			if(strtomp(rpc->arg, nil, 16, n) != nil
			&& mpcmp(n, rsapub->n) == 0){
				mpfree(n);
				return rpc;
			}
		}
		mpfree(n);
	}
	factotum_rsa_close(rpc);
	return nil;
}

static mpint*
factotum_rsa_decrypt(AuthRpc *rpc, mpint *cipher)
{
	char *p;
	int rv;

	if(cipher == nil)
		return nil;
	p = mptoa(cipher, 16, nil, 0);
	mpfree(cipher);
	if(p == nil)
		return nil;
	rv = auth_rpc(rpc, "write", p, strlen(p));
	free(p);
	if(rv != ARok || auth_rpc(rpc, "read", nil, 0) != ARok)
		return nil;
	return strtomp(rpc->arg, nil, 16, nil);
}

static void
factotum_rsa_close(AuthRpc *rpc)
{
	if(rpc == nil)
		return;
	close(rpc->afd);
	auth_freerpc(rpc);
}

// buf ^= prf
static void
tlsP(uchar *buf, int nbuf, uchar *key, int nkey, uchar *label, int nlabel, uchar *seed, int nseed,
	DigestState* (*x)(uchar*, ulong, uchar*, ulong, uchar*, DigestState*), int xlen)
{
	uchar ai[SHA2_256dlen], tmp[SHA2_256dlen];
	DigestState *s;
	int n, i;

	assert(xlen <= (int)sizeof(ai) && xlen <= (int)sizeof(tmp));
	// generate a1
	s = x(label, nlabel, key, nkey, nil, nil);
	x(seed, nseed, key, nkey, ai, s);

	while(nbuf > 0) {
		s = x(ai, xlen, key, nkey, nil, nil);
		s = x(label, nlabel, key, nkey, nil, s);
		x(seed, nseed, key, nkey, tmp, s);
		n = xlen;
		if(n > nbuf)
			n = nbuf;
		for(i = 0; i < n; i++)
			buf[i] ^= tmp[i];
		buf += n;
		nbuf -= n;
		x(ai, xlen, key, nkey, tmp, nil);
		memmove(ai, tmp, xlen);
	}
}


// fill buf with md5(args)^sha1(args)
static void
tls10PRF(uchar *buf, int nbuf, uchar *key, int nkey, char *label, uchar *seed, int nseed)
{
	int nlabel = strlen(label);
	int n = (nkey + 1) >> 1;

	memset(buf, 0, nbuf);
	tlsP(buf, nbuf, key, n, (uchar*)label, nlabel, seed, nseed,
		hmac_md5, MD5dlen);
	tlsP(buf, nbuf, key+nkey-n, n, (uchar*)label, nlabel, seed, nseed,
		hmac_sha1, SHA1dlen);
}

static void
tls12PRF(uchar *buf, int nbuf, uchar *key, int nkey, char *label, uchar *seed, int nseed)
{
	memset(buf, 0, nbuf);
	tlsP(buf, nbuf, key, nkey, (uchar*)label, strlen(label), seed, nseed,
		hmac_sha2_256, SHA2_256dlen);
}

static void
sslPRF(uchar *buf, int nbuf, uchar *key, int nkey, char *label, uchar *seed, int nseed)
{
	uchar sha1dig[SHA1dlen], md5dig[MD5dlen], tmp[26];
	DigestState *s;
	int i, n, len;

	USED(label);
	len = 1;
	while(nbuf > 0){
		if(len > 26)
			return;
		for(i = 0; i < len; i++)
			tmp[i] = 'A' - 1 + len;
		s = sha1(tmp, len, nil, nil);
		s = sha1(key, nkey, nil, s);
		sha1(seed, nseed, sha1dig, s);
		s = md5(key, nkey, nil, nil);
		md5(sha1dig, SHA1dlen, md5dig, s);
		n = MD5dlen;
		if(n > nbuf)
			n = nbuf;
		memmove(buf, md5dig, n);
		buf += n;
		nbuf -= n;
		len++;
	}
}

static void
sslSetFinished(TlsSec *sec, HandshakeHash hsh, uchar *finished, int isclient)
{
	DigestState *s;
	uchar h0[MD5dlen], h1[SHA1dlen], pad[48];
	char *label;

	if(isclient)
		label = "CLNT";
	else
		label = "SRVR";

	md5((uchar*)label, 4, nil, &hsh.md5);
	md5(sec->sec, MasterSecretSize, nil, &hsh.md5);
	memset(pad, 0x36, 48);
	md5(pad, 48, nil, &hsh.md5);
	md5(nil, 0, h0, &hsh.md5);
	memset(pad, 0x5C, 48);
	s = md5(sec->sec, MasterSecretSize, nil, nil);
	s = md5(pad, 48, nil, s);
	md5(h0, MD5dlen, finished, s);

	sha1((uchar*)label, 4, nil, &hsh.sha1);
	sha1(sec->sec, MasterSecretSize, nil, &hsh.sha1);
	memset(pad, 0x36, 40);
	sha1(pad, 40, nil, &hsh.sha1);
	sha1(nil, 0, h1, &hsh.sha1);
	memset(pad, 0x5C, 40);
	s = sha1(sec->sec, MasterSecretSize, nil, nil);
	s = sha1(pad, 40, nil, s);
	sha1(h1, SHA1dlen, finished + MD5dlen, s);
}

// fill "finished" arg with md5(args)^sha1(args)
static void
tls10SetFinished(TlsSec *sec, HandshakeHash hsh, uchar *finished, int isclient)
{
	uchar h[MD5dlen+SHA1dlen];
	char *label;

	// get current hash value, but allow further messages to be hashed in
	md5(nil, 0, h, &hsh.md5);
	sha1(nil, 0, h+MD5dlen, &hsh.sha1);

	if(isclient)
		label = "client finished";
	else
		label = "server finished";
	tls10PRF(finished, TLSFinishedLen, sec->sec, MasterSecretSize, label, h, sizeof(h));
}

static void
tls12SetFinished(TlsSec *sec, HandshakeHash hsh, uchar *finished, int isclient)
{
	uchar seed[SHA2_256dlen];
	char *label;

	// get current hash value, but allow further messages to be hashed in
	sha2_256(nil, 0, seed, &hsh.sha2_256);

	if(isclient)
		label = "client finished";
	else
		label = "server finished";
	tls12PRF(finished, TLSFinishedLen, sec->sec, MasterSecretSize, label, seed, SHA2_256dlen);
}

static void
tlsSecInits(TlsSec *sec, int cvers, uchar *crandom)
{
	memset(sec, 0, sizeof(*sec));
	sec->clientVers = cvers;
	memmove(sec->crandom, crandom, RandomSize);

	// putting time()'s output to the first 4 bytes is no
	// longer recommended and is not useful
	genrandom(sec->srandom, RandomSize);
}

static int
tlsSecRSAs(TlsSec *sec, Bytes *epm)
{
	Bytes *pm;

	if(epm == nil){
		werrstr("no encrypted premaster secret");
		return -1;
	}
	// if the client messed up, just continue as if everything is ok,
	// to prevent attacks to check for correctly formatted messages.
	pm = pkcs1_decrypt(sec, epm);
	if(pm == nil || pm->len != MasterSecretSize || get16(pm->data) != sec->clientVers){
		freebytes(pm);
		pm = newbytes(MasterSecretSize);
		genrandom(pm->data, pm->len);
	}
	setMasterSecret(sec, pm);
	return 0;
}

static Bytes*
tlsSecECDHEs1(TlsSec *sec)
{
	ECdomain *dom = &sec->ec.dom;
	ECpriv *Q = &sec->ec.Q;
	Bytes *par;
	int n;

	if(sec->nc == nil)
		return nil;
	if(sec->nc->tlsid == X25519){
		par = newbytes(1+2+1+32);
		par->data[0] = 3;
		put16(par->data+1, X25519);
		par->data[3] = 32;
		curve25519_dh_new(sec->X, par->data+4);
	}else{
		ecdominit(dom, sec->nc->init);
		memset(Q, 0, sizeof(*Q));
		Q->a.x = mpnew(0);
		Q->a.y = mpnew(0);
		Q->d = mpnew(0);
		ecgen(dom, Q);
		n = 1 + 2*((mpsignif(dom->p)+7)/8);
		par = newbytes(1+2+1+n);
		par->data[0] = 3;
		put16(par->data+1, sec->nc->tlsid);
		n = ecencodepub(dom, &Q->a, par->data+4, par->len-4);
		par->data[3] = n;
		par->len = 1+2+1+n;
	}
	return par;
}

static int
tlsSecECDHEs2(TlsSec *sec, Bytes *Yc)
{
	ECdomain *dom = &sec->ec.dom;
	ECpriv *Q = &sec->ec.Q;
	ECpoint K;
	ECpub *Y;
	Bytes *Z;

	if(Yc == nil){
		werrstr("no public key");
		return -1;
	}

	if(sec->nc->tlsid == X25519){
		if(Yc->len != 32){
			werrstr("bad public key");
			return -1;
		}
		Z = newbytes(32);
		if(!curve25519_dh_finish(sec->X, Yc->data, Z->data)){
			werrstr("unlucky shared key");
			freebytes(Z);
			return -1;
		}
		setMasterSecret(sec, Z);
	}else{
		if((Y = ecdecodepub(dom, Yc->data, Yc->len)) == nil){
			werrstr("bad public key");
			return -1;
		}

		memset(&K, 0, sizeof(K));
		K.x = mpnew(0);
		K.y = mpnew(0);

		ecmul(dom, Y, Q->d, &K);

		setMasterSecret(sec, mptobytes(K.x, (mpsignif(dom->p)+7)/8));

		mpfree(K.x);
		mpfree(K.y);

		ecpubfree(Y);
	}
	return 0;
}

static void
tlsSecInitc(TlsSec *sec, int cvers)
{
	memset(sec, 0, sizeof(*sec));
	sec->clientVers = cvers;
	// see the comment on tlsSecInits
	genrandom(sec->crandom, RandomSize);
}

static Bytes*
tlsSecRSAc(TlsSec *sec, uchar *cert, int ncert)
{
	RSApub *pub;
	Bytes *pm, *epm;

	pub = X509toRSApub(cert, ncert, nil, 0);
	if(pub == nil){
		werrstr("invalid x509/rsa certificate");
		return nil;
	}
	pm = newbytes(MasterSecretSize);
	put16(pm->data, sec->clientVers);
	genrandom(pm->data+2, MasterSecretSize - 2);
	epm = pkcs1_encrypt(pm, pub);
	setMasterSecret(sec, pm);
	rsapubfree(pub);
	return epm;
}

static int
tlsSecFinished(TlsSec *sec, HandshakeHash hsh, uchar *fin, int nfin, int isclient)
{
	if(sec->nfin != nfin){
		werrstr("invalid finished exchange");
		return -1;
	}
	hsh.md5.malloced = 0;
	hsh.sha1.malloced = 0;
	hsh.sha2_256.malloced = 0;
	(*sec->setFinished)(sec, hsh, fin, isclient);
	return 0;
}

static void
tlsSecVers(TlsSec *sec, int v)
{
	if(v == SSL3Version){
		sec->setFinished = sslSetFinished;
		sec->nfin = SSL3FinishedLen;
		sec->prf = sslPRF;
	}else if(v < TLS12Version) {
		sec->setFinished = tls10SetFinished;
		sec->nfin = TLSFinishedLen;
		sec->prf = tls10PRF;
	}else {
		sec->setFinished = tls12SetFinished;
		sec->nfin = TLSFinishedLen;
		sec->prf = tls12PRF;
	}
}

static int
setSecrets(TlsConnection *c, int isclient)
{
	uchar kd[MaxKeyData], seed[2*RandomSize];
	char *secrets;
	int rv;

	assert(c->nsecret <= (int)sizeof(kd));
	secrets = emalloc(2*c->nsecret);

	memmove(seed, c->sec->srandom, RandomSize);
	memmove(seed+RandomSize, c->sec->crandom, RandomSize);
	/*
	 * generate secret keys from the master secret.
	 *
	 * different cipher selections will require different amounts
	 * of key expansion and use of key expansion data,
	 * but it's all generated using the same function.
	 */
	(*c->sec->prf)(kd, c->nsecret, c->sec->sec, MasterSecretSize, "key expansion",
			seed, sizeof(seed));

	enc64(secrets, 2*c->nsecret, kd, c->nsecret);
	memset(kd, 0, c->nsecret);

	rv = fprint(c->ctl, "secret %s %s %d %s", c->digest, c->enc, isclient, secrets);
	memset(secrets, 0, 2*c->nsecret);
	free(secrets);

	return rv;
}

/*
 * set the master secret from the pre-master secret,
 * destroys premaster.
 */
static void
setMasterSecret(TlsSec *sec, Bytes *pm)
{
	uchar seed[2*RandomSize];

	if(sec->psklen > 0){
		Bytes *opm = pm;
		uchar *p;

		/* concatenate psk to pre-master secret */
		pm = newbytes(4 + opm->len + sec->psklen);
		p = pm->data;
		put16(p, opm->len), p += 2;
		memmove(p, opm->data, opm->len), p += opm->len;
		put16(p, sec->psklen), p += 2;
		memmove(p, sec->psk, sec->psklen);

		memset(opm->data, 0, opm->len);
		freebytes(opm);
	}

	memmove(seed, sec->crandom, RandomSize);
	memmove(seed+RandomSize, sec->srandom, RandomSize);
	(*sec->prf)(sec->sec, MasterSecretSize, pm->data, pm->len, "master secret",
			seed, sizeof(seed));

	memset(pm->data, 0, pm->len);	
	freebytes(pm);
}

static int
digestDHparams(TlsSec *sec, Bytes *par, uchar digest[MAXdlen], int sigalg)
{
	uint hashalg = (sigalg>>8) & 0xFF;
	int digestlen;
	Bytes *blob;

	blob = newbytes(2*RandomSize + par->len);
	memmove(blob->data+0*RandomSize, sec->crandom, RandomSize);
	memmove(blob->data+1*RandomSize, sec->srandom, RandomSize);
	memmove(blob->data+2*RandomSize, par->data, par->len);
	if(hashalg == 0){
		digestlen = MD5dlen+SHA1dlen;
		md5(blob->data, blob->len, digest, nil);
		sha1(blob->data, blob->len, digest+MD5dlen, nil);
	} else {
		digestlen = -1;
		if(hashalg < nelem(hashfun) && hashfun[hashalg].fun != nil){
			digestlen = hashfun[hashalg].len;
			(*hashfun[hashalg].fun)(blob->data, blob->len, digest, nil);
		}
	}
	freebytes(blob);
	return digestlen;
}

static char*
verifyDHparams(TlsSec *sec, Bytes *par, Bytes *cert, Bytes *sig, int sigalg)
{
	uchar digest[MAXdlen];
	int digestlen;
	ECdomain dom;
	ECpub *ecpk;
	RSApub *rsapk;
	char *err;

	if(par == nil || par->len <= 0)
		return "no DH parameters";

	if(sig == nil || sig->len <= 0){
		if(sec->psklen > 0)
			return nil;
		return "no signature";
	}

	if(cert == nil)
		return "no certificate";

	digestlen = digestDHparams(sec, par, digest, sigalg);
	if(digestlen <= 0)
		return "unknown signature digest algorithm";
	
	switch(sigalg & 0xFF){
	case 0x01:
		rsapk = X509toRSApub(cert->data, cert->len, nil, 0);
		if(rsapk == nil)
			return "bad certificate";
		err = X509rsaverifydigest(sig->data, sig->len, digest, digestlen, rsapk);
		rsapubfree(rsapk);
		break;
	case 0x03:
		ecpk = X509toECpub(cert->data, cert->len, nil, 0, &dom);
		if(ecpk == nil)
			return "bad certificate";
		err = X509ecdsaverifydigest(sig->data, sig->len, digest, digestlen, &dom, ecpk);
		ecdomfree(&dom);
		ecpubfree(ecpk);
		break;
	default:
		err = "signaure algorithm not RSA or ECDSA";
	}

	return err;
}

// encrypt data according to PKCS#1, /lib/rfc/rfc2437 9.1.2.1
static Bytes*
pkcs1_encrypt(Bytes* data, RSApub* key)
{
	mpint *x, *y;

	x = pkcs1padbuf(data->data, data->len, key->n, 2);
	if(x == nil)
		return nil;
	y = rsaencrypt(key, x, nil);
	mpfree(x);
	data = newbytes((mpsignif(key->n)+7)/8);
	mptober(y, data->data, data->len);
	mpfree(y);
	return data;
}

// decrypt data according to PKCS#1, with given key.
static Bytes*
pkcs1_decrypt(TlsSec *sec, Bytes *data)
{
	mpint *y;

	if(data->len != (mpsignif(sec->rsapub->n)+7)/8)
		return nil;
	y = factotum_rsa_decrypt(sec->rpc, bytestomp(data));
	if(y == nil)
		return nil;
	data = mptobytes(y, (mpsignif(y)+7)/8);
	mpfree(y);
	if((data->len = pkcs1unpadbuf(data->data, data->len, sec->rsapub->n, 2)) < 0){
		freebytes(data);
		return nil;
	}
	return data;
}

static Bytes*
pkcs1_sign(TlsSec *sec, uchar *digest, int digestlen, int sigalg)
{
	uint hashalg = (sigalg>>8)&0xFF;
	mpint *signedMP;
	Bytes *signature;
	uchar buf[128];

	if(hashalg > 0 && hashalg < nelem(hashfun) && hashfun[hashalg].len == digestlen)
		digestlen = asn1encodedigest(hashfun[hashalg].fun, digest, buf, sizeof(buf));
	else if(digestlen == MD5dlen+SHA1dlen)
		memmove(buf, digest, digestlen);
	else
		digestlen = -1;
	if(digestlen <= 0){
		werrstr("bad digest algorithm");
		return nil;
	}

	signedMP = factotum_rsa_decrypt(sec->rpc, pkcs1padbuf(buf, digestlen, sec->rsapub->n, 1));
	if(signedMP == nil)
		return nil;
	signature = mptobytes(signedMP, (mpsignif(sec->rsapub->n)+7)/8);
	mpfree(signedMP);
	return signature;
}


//================= general utility functions ========================

static void *
emalloc(int n)
{
	void *p;
	if(n==0)
		n=1;
	p = malloc(n);
	if(p == nil)
		sysfatal("out of memory");
	memset(p, 0, n);
	setmalloctag(p, getcallerpc(&n));
	return p;
}

static void *
erealloc(void *ReallocP, int ReallocN)
{
	if(ReallocN == 0)
		ReallocN = 1;
	if(ReallocP == nil)
		ReallocP = emalloc(ReallocN);
	else if((ReallocP = realloc(ReallocP, ReallocN)) == nil)
		sysfatal("out of memory");
	setrealloctag(ReallocP, getcallerpc(&ReallocP));
	return(ReallocP);
}

/*
static void
put32(uchar *p, u32int x)
{
	p[0] = x>>24;
	p[1] = x>>16;
	p[2] = x>>8;
	p[3] = x;
}
*/

static void
put24(uchar *p, int x)
{
	p[0] = x>>16;
	p[1] = x>>8;
	p[2] = x;
}

static void
put16(uchar *p, int x)
{
	p[0] = x>>8;
	p[1] = x;
}

static int
get24(uchar *p)
{
	return (p[0]<<16)|(p[1]<<8)|p[2];
}

static int
get16(uchar *p)
{
	return (p[0]<<8)|p[1];
}

static Bytes*
newbytes(int len)
{
	Bytes* ans;

	if(len < 0)
		abort();
	ans = emalloc(sizeof(Bytes) + len);
	ans->len = len;
	return ans;
}

/*
 * newbytes(len), with data initialized from buf
 */
static Bytes*
makebytes(uchar* buf, int len)
{
	Bytes* ans;

	ans = newbytes(len);
	memmove(ans->data, buf, len);
	return ans;
}

static void
freebytes(Bytes* b)
{
	free(b);
}

static mpint*
bytestomp(Bytes* bytes)
{
	return betomp(bytes->data, bytes->len, nil);
}

/*
 * Convert mpint* to Bytes, putting high order byte first.
 */
static Bytes*
mptobytes(mpint *big, int len)
{
	Bytes* ans;

	if(len == 0) len++;
	ans = newbytes(len);
	mptober(big, ans->data, ans->len);
	return ans;
}

/* len is number of ints */
static Ints*
newints(int len)
{
	Ints* ans;

	if(len < 0 || (uint)len > ((uint)-1>>1)/sizeof(int))
		abort();
	ans = emalloc(sizeof(Ints) + len*sizeof(int));
	ans->len = len;
	return ans;
}

static void
freeints(Ints* b)
{
	free(b);
}

static int
lookupid(Ints* b, int id)
{
	int i;

	for(i=0; i<b->len; i++)
		if(b->data[i] == id)
			return i;
	return -1;
}
