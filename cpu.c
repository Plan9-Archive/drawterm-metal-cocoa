/*
 * cpu.c - Make a connection to a cpu server
 */

#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <authsrv.h>
#include <libsec.h>
#include "args.h"
#include "drawterm.h"

#define MaxStr 128

static void	usage(void);
static void	writestr(int, char*, char*, int);
static int	readstr(int, char*, int);
static char 	*keyspec = "";
static AuthInfo *p9any(int);
static int	getkey(Authkey*, char*, char*, char*, char*);
static int	findkey(Authkey*, char*, char*, char*);

static char	*host;
static int	aanfilter;
static int	aanto = 3600 * 24;
static int	norcpu;
static int	nokbd;
static int	nogfx;
static int	nineflag;

static char	*ealgs = "rc4_256 sha1";

/* authentication mechanisms */
static int	p9authssl(int);
static int	p9authtls(int);

char *authserver;
char *secstore;
char *user, *pass;
char secstorebuf[65536];
char *geometry;

extern void	guimain(void);

char*
estrdup(char *s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("out of memory");
	return s;
}

static void
ending(void)
{
	panic("ending");
}

int
mountfactotum(void)
{
	int fd;
	
	if((fd = dialfactotum()) < 0)
		return -1;
	if(sysmount(fd, -1, "/mnt/factotum", MREPL, "") < 0){
		fprint(2, "mount factotum: %r\n");
		return -1;
	}
	if((fd = open("/mnt/factotum/ctl", OREAD)) < 0){
		fprint(2, "open /mnt/factotum/ctl: %r\n");
		return -1;
	}
	close(fd);
	return 0;
}

static int
startaan(char *host, int fd)
{
	static char script[] =
"~ $#netdir 1 || netdir=/net/tcp/clone\n"
"netdir=`{basename -d $netdir} || exit\n"
"<>$netdir/clone {\n"
"	netdir=$netdir/`{read} || exit\n"
"	>[3] $netdir/ctl {\n"
"		echo -n 'announce *!0' >[1=3]\n"
"		echo `{cat $netdir/local} || exit\n"
"		bind '#|' /mnt/aan || exit\n"
"		exec aan -m $aanto $netdir <>/mnt/aan/data1 >[1=0] >[2]/dev/null &\n"
"	}\n"
"}\n"
"<>/mnt/aan/data >[1=0] >[2]/dev/null {\n"
"	rfork n\n"
"	fn server {\n"
"		echo -n aanserver $netdir >/proc/$pid/args\n"
"		rm -f /env/^('fn#server' aanto)\n"
"		. <{n=`{read} && ! ~ $#n 0 && read -c $n} >[2=1]\n"
"	}\n"
"	exec tlssrv -A /bin/rc -c server\n"
"	exit\n"
"}\n";
	char buf[128], *p, *na;
	int n;

	snprint(buf, sizeof buf, "aanto=%d\n", aanto);
	if(fprint(fd, "%7ld\n%s%s", strlen(buf)+strlen(script), buf, script) < 0)
		sysfatal("sending aan script: %r");
	n = read(fd, buf, sizeof(buf)-1);
	close(fd);

	while(n > 0 && buf[n-1] == '\n') n--;
	if(n <= 0) return -1;
	buf[n] = 0;
	if((p = strrchr(buf, '!')) != nil)
		na = strdup(netmkaddr(host, "tcp", p+1));
	else
		na = strdup(buf);

	return aanclient(na, aanto);
}

static void
rcpuexit(void)
{
	char *s = getenv("rstatus");
	if(s != nil)
		exit(*s);
}

void
rcpu(char *host, char *cmd)
{
	static char script[] = 
"mount -nc /fd/0 /mnt/term || exit\n"
"bind -q /mnt/term/dev/cons /dev/cons\n"
"if(test -r /mnt/term/dev/kbd){\n"
"	</dev/cons >/dev/cons >[2=1] aux/kbdfs -dq -m /mnt/term/dev\n"
"	bind -q /mnt/term/dev/cons /dev/cons\n"
"}\n"
"</dev/cons >/dev/cons >[2=1] service=cpu %s\n"
"echo -n $status >/mnt/term/env/rstatus >[2]/dev/null\n"
"echo -n hangup >/proc/$pid/notepg\n";
	int fd;

	if((fd = dial(netmkaddr(host, "tcp", "rcpu"), nil, nil, nil)) < 0)
		return;

	/* provide /dev/kbd for kbdfs */
	if(!nokbd)
		bind("#b", "/dev", MAFTER);

	fd = p9authtls(fd);
	if(aanfilter){
		fd = startaan(host, fd);
		if(fd < 0)
			sysfatal("startaan: %r");
		fd = p9authtls(fd);
	}
	memset(secstorebuf, 0, sizeof(secstorebuf));	/* forget secstore secrets */

	if(cmd == nil)
		cmd = smprint(script, "rc -li");
	else {
		char *run = smprint("rc -lc %q", cmd);
		cmd = smprint(script, run);
		free(run);
	}
	if(fprint(fd, "%7ld\n%s", strlen(cmd), cmd) < 0)
		sysfatal("sending script: %r");
	free(cmd);

	/* /env/rstatus is written by the remote script to communicate exit status */
	remove("/env/rstatus");
	atexit(rcpuexit);

	/* Begin serving the namespace */
	exportfs(fd, fd);
}

void
ncpu(char *host, char *cmd)
{
	char buf[MaxStr];
	int fd;

	if((fd = dial(netmkaddr(host, "tcp", "ncpu"), nil, nil, nil)) < 0)
		return;

	/* negotiate authentication mechanism */
	strcpy(buf, "p9");
	if(ealgs != nil){
		strcat(buf, " ");
		strcat(buf, ealgs);
	}
	writestr(fd, buf, "negotiating authentication method", 0);
	if(readstr(fd, buf, sizeof buf) < 0)
		sysfatal("can't negotiate authentication method: %r");
	if(*buf)
		sysfatal("%s", buf);

	/* authenticate and encrypt the channel */
	fd = p9authssl(fd);

	/* tell remote side the command */
	if(cmd != nil){
		cmd = smprint("!%s", cmd);
		writestr(fd, cmd, "cmd", 0);
		free(cmd);
	}

	/* Tell the remote side where our working directory is */
	if(getcwd(buf, sizeof(buf)) == 0)
		writestr(fd, "NO", "dir", 0);
	else
		writestr(fd, buf, "dir", 0);

	/* 
	 *  Wait for the other end to execute and start our file service
	 *  of /mnt/term
	 */
	if(readstr(fd, buf, sizeof(buf)) < 0)
		sysfatal("waiting for FS: %r");
	if(strncmp("FS", buf, 2) != 0) {
		print("remote cpu: %s", buf);
		exits(buf);
	}

	if(readstr(fd, buf, sizeof(buf)) < 0)
		sysfatal("waiting for remote export: %r");
	if(strcmp(buf, "/") != 0){
		print("remote cpu: %s", buf);
		exits(buf);
	}
	write(fd, "OK", 2);

	/* Begin serving the gnot namespace */
	exportfs(fd, fd);
}

void
usage(void)
{
	fprint(2, "usage: %s [-9GBO] "
		"[-h host] [-u user] [-a authserver] [-s secstore] "
		"[-e 'crypt hash'] [-k keypattern] "
		"[-p] [-t timeout] "
		"[-r root] "
		"[-g geometry] "
		"[-c cmd ...]\n", argv0);
	exits("usage");
}

static char *cmd;

extern void cpubody(void);

void
cpumain(int argc, char **argv)
{
	char *s;

	user = getenv("USER");
	host = getenv("cpu");
	authserver = getenv("auth");

	ARGBEGIN{
	case '9':
		nineflag = 1;
		/* wet floor */
	case 'G':
		nogfx = 1;
		/* wet floor */
	case 'B':
		nokbd = 1;
		break;
	case 'O':
		norcpu = 1;
		break;
	case 'p':
		aanfilter = 1;
		break;
	case 't':
		aanto = (int)strtol(EARGF(usage()), nil, 0);
		break;
	case 'h':
		host = EARGF(usage());
		break;
	case 'a':
		authserver = EARGF(usage());
		break;
	case 's':
		secstore = EARGF(usage());
		break;
	case 'e':
		ealgs = EARGF(usage());
		if(*ealgs == 0 || strcmp(ealgs, "clear") == 0)
			ealgs = nil;
		break;
	case 'k':
		keyspec = EARGF(usage());
		break;
	case 'u':
		user = EARGF(usage());
		break;
	case 'r':
		s = smprint("/root/%s", EARGF(usage()));
		cleanname(s);
		if(bind(s, "/root", MREPL) < 0)
			panic("bind /root: %r");
		free(s);
		break;
	case 'c':
		cmd = estrdup(EARGF(usage()));
		while((s = ARGF()) != nil){
			char *old = cmd;
			cmd = smprint("%s %q", cmd, s);
			free(old);
		}
		break;
	case 'g':
		/* X11 geometry string
		 *	[=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
		 */
		geometry = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if(argc != 0)
		usage();

	if(nineflag){
		exportfs(lfdfd(0), lfdfd(1));
		return;
	}

	if((pass = getenv("PASS")) != nil)
		remove("/env/PASS");

	if(!nogfx)
		guimain();
	else
		cpubody();
}

void
cpubody(void)
{
	char *s;

	if(!nogfx){
		if(bind("#i", "/dev", MBEFORE) < 0)
			panic("bind #i: %r");
		if(bind("#m", "/dev", MBEFORE) < 0)
			panic("bind #m: %r");
		if(cmd == nil)
			atexit(ending);
	}
	if(bind("/root", "/", MAFTER) < 0)
		panic("bind /root: %r");

	if(host == nil)
		if((host = readcons("cpu", "cpu", 0)) == nil)
			sysfatal("user terminated input");

	if(authserver == nil)
		if((authserver = readcons("auth", host, 0)) == nil)
			sysfatal("user terminated input");

	if(user == nil)
		if((user = readcons("user", "glenda", 0)) == nil)
			sysfatal("user terminated input");

	if(mountfactotum() < 0){
		if(secstore == nil)
			secstore = authserver;
	 	if(havesecstore(secstore, user)){
			s = secstorefetch(secstore, user, pass);
			if(s){
				if(strlen(s) >= sizeof secstorebuf)
					sysfatal("secstore data too big");
				strcpy(secstorebuf, s);
			}
		}
	}

	if(!norcpu)
		rcpu(host, cmd);

	ncpu(host, cmd);

	sysfatal("can't dial %s: %r", host);
}

void
writestr(int fd, char *str, char *thing, int ignore)
{
	int l, n;

	l = strlen(str);
	n = write(fd, str, l+1);
	if(!ignore && n < 0)
		sysfatal("writing network: %s: %r", thing);
}

int
readstr(int fd, char *str, int len)
{
	int n;

	while(len) {
		n = read(fd, str, 1);
		if(n < 0) 
			return -1;
		if(n == 0){
			werrstr("hung up");
			return -1;
		}
		if(*str == '\0')
			return 0;
		str++;
		len--;
	}
	return -1;
}

static void
mksecret(char *t, uchar *f)
{
	sprint(t, "%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux",
		f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
}

/*
 *  plan9 authentication followed by rc4 encryption
 */
static int
p9authssl(int fd)
{
	uchar key[16];
	uchar digest[SHA1dlen];
	char fromclientsecret[21];
	char fromserversecret[21];
	AuthInfo *ai;

	ai = p9any(fd);
	memset(secstorebuf, 0, sizeof(secstorebuf));	/* forget secstore secrets */
	if(ai == nil)
		sysfatal("can't authenticate: %r");

	if(ealgs == nil)
		return fd;

	if(ai->nsecret < 8){
		sysfatal("p9authssl: secret too small");
		return -1;
	}
	memmove(key+4, ai->secret, 8);

	/* exchange random numbers */
	genrandom(key, 4);
	if(write(fd, key, 4) != 4){
		sysfatal("p9authssl: write random: %r");
		return -1;
	}
	if(readn(fd, key+12, 4) != 4){
		sysfatal("p9authssl: read random: %r");
		return -1;
	}

	/* scramble into two secrets */
	sha1(key, sizeof(key), digest, nil);
	mksecret(fromclientsecret, digest);
	mksecret(fromserversecret, digest+10);

	/* set up encryption */
	fd = pushssl(fd, ealgs, fromclientsecret, fromserversecret, nil);
	if(fd < 0)
		sysfatal("p9authssl: pushssl: %r");

	return fd;
}

/*
 * p9any authentication followed by tls-psk encryption
 */
static int
p9authtls(int fd)
{
	AuthInfo *ai;
	TLSconn *conn;

	ai = p9any(fd);
	if(ai == nil)
		sysfatal("can't authenticate: %r");

	conn = mallocz(sizeof(TLSconn), 1);
	conn->pskID = "p9secret";
	conn->psk = ai->secret;
	conn->psklen = ai->nsecret;

	fd = tlsClient(fd, conn);
	if(fd < 0)
		sysfatal("tlsClient: %r");

	auth_freeAI(ai);
	free(conn->sessionID);
	free(conn);

	return fd;
}

int
authdial(char *net, char *dom)
{
	return dial(netmkaddr(authserver, "tcp", "ticket"), 0, 0, 0);
}

static int
getastickets(Authkey *key, Ticketreq *tr, uchar *y, char *tbuf, int tbuflen)
{
	int asfd, rv;
	char *dom;

	dom = tr->authdom;
	asfd = authdial(nil, dom);
	if(asfd < 0)
		return -1;
	if(y != nil){
		PAKpriv p;

		rv = -1;
		tr->type = AuthPAK;
		if(_asrequest(asfd, tr) != 0 || write(asfd, y, PAKYLEN) != PAKYLEN)
			goto Out;

		authpak_new(&p, key, (uchar*)tbuf, 1);
		if(write(asfd, tbuf, PAKYLEN) != PAKYLEN)
			goto Out;

		if(_asrdresp(asfd, tbuf, 2*PAKYLEN) != 2*PAKYLEN)
			goto Out;
	
		memmove(y, tbuf, PAKYLEN);
		if(authpak_finish(&p, key, (uchar*)tbuf+PAKYLEN))
			goto Out;
	}
	tr->type = AuthTreq;
	rv = _asgetticket(asfd, tr, tbuf, tbuflen);
Out:
	close(asfd);
	return rv;
}

static int
mkservertickets(Authkey *key, Ticketreq *tr, uchar *y, char *tbuf, int tbuflen)
{
	Ticket t;
	int ret;

	if(strcmp(tr->authid, tr->hostid) != 0)
		return -1;
	memset(&t, 0, sizeof(t));
	ret = 0;
	if(y != nil){
		PAKpriv p;

		t.form = 1;
		memmove(tbuf, y, PAKYLEN);
		authpak_new(&p, key, y, 0);
		authpak_finish(&p, key, (uchar*)tbuf);
	}
	memmove(t.chal, tr->chal, CHALLEN);
	strcpy(t.cuid, tr->uid);
	strcpy(t.suid, tr->uid);
	genrandom((uchar*)t.key, sizeof(t.key));
	t.num = AuthTc;
	ret += convT2M(&t, tbuf+ret, tbuflen-ret, key);
	t.num = AuthTs;
	ret += convT2M(&t, tbuf+ret, tbuflen-ret, key);
	memset(&t, 0, sizeof(t));

	return ret;
}

static int
gettickets(Authkey *key, Ticketreq *tr, uchar *y, char *tbuf, int tbuflen)
{
	int ret;
	ret = getastickets(key, tr, y, tbuf, tbuflen);
	if(ret > 0)
		return ret;
	return mkservertickets(key, tr, y, tbuf, tbuflen);
}

/*
 *  prompt user for a key.  don't care about memory leaks, runs standalone
 */
static Attr*
promptforkey(char *params)
{
	char *v;
	int fd;
	Attr *a, *attr;
	char *def;

	fd = open("/dev/cons", ORDWR);
	if(fd < 0)
		sysfatal("opening /dev/cons: %r");

	attr = _parseattr(params);
	fprint(fd, "\n!Adding key:");
	for(a=attr; a; a=a->next)
		if(a->type != AttrQuery && a->name[0] != '!')
			fprint(fd, " %q=%q", a->name, a->val);
	fprint(fd, "\n");

	for(a=attr; a; a=a->next){
		v = a->name;
		if(a->type != AttrQuery || v[0]=='!')
			continue;
		def = nil;
		if(strcmp(v, "user") == 0)
			def = getuser();
		a->val = readcons(v, def, 0);
		if(a->val == nil)
			sysfatal("user terminated key input");
		a->type = AttrNameval;
	}
	for(a=attr; a; a=a->next){
		v = a->name;
		if(a->type != AttrQuery || v[0]!='!')
			continue;
		def = nil;
		if(strcmp(v+1, "user") == 0)
			def = getuser();
		a->val = readcons(v+1, def, 1);
		if(a->val == nil)
			sysfatal("user terminated key input");
		a->type = AttrNameval;
	}
	fprint(fd, "!\n");
	close(fd);
	return attr;
}

/*
 *  send a key to the mounted factotum
 */
static int
sendkey(Attr *attr)
{
	int fd, rv;
	char buf[1024];

	fd = open("/mnt/factotum/ctl", ORDWR);
	if(fd < 0)
		sysfatal("opening /mnt/factotum/ctl: %r");
	rv = fprint(fd, "key %A\n", attr);
	read(fd, buf, sizeof buf);
	close(fd);
	return rv;
}

int
askuser(char *params)
{
	Attr *attr;
	
	fmtinstall('A', _attrfmt);
	
	attr = promptforkey(params);
	if(attr == nil)
		sysfatal("no key supplied");
	if(sendkey(attr) < 0)
		sysfatal("sending key to factotum: %r");
	return 0;
}

AuthInfo*
p9anyfactotum(int fd, int afd)
{
	return auth_proxy(fd, askuser, "proto=p9any role=client %s", keyspec);
}

AuthInfo*
p9any(int fd)
{
	char buf[1024], buf2[1024], *bbuf, *p, *proto, *dom;
	uchar crand[2*NONCELEN], cchal[CHALLEN], y[PAKYLEN];
	char tbuf[2*MAXTICKETLEN+MAXAUTHENTLEN+PAKYLEN], trbuf[TICKREQLEN+PAKYLEN];
	Authkey authkey;
	Authenticator auth;
	int afd, i, n, m, v2, dp9ik;
	Ticketreq tr;
	Ticket t;
	AuthInfo *ai;

	if((afd = open("/mnt/factotum/ctl", ORDWR)) >= 0)
		return p9anyfactotum(fd, afd);
	werrstr("");

	if(readstr(fd, buf, sizeof buf) < 0)
		sysfatal("cannot read p9any negotiation: %r");
	bbuf = buf;
	v2 = 0;
	if(strncmp(buf, "v.2 ", 4) == 0){
		v2 = 1;
		bbuf += 4;
	}
	dp9ik = 0;
	proto = nil;
	while(bbuf != nil){
		if((p = strchr(bbuf, ' ')))
			*p++ = 0;
		if((dom = strchr(bbuf, '@')) == nil)
			sysfatal("bad p9any domain");
		*dom++ = 0;
		if(strcmp(bbuf, "p9sk1") == 0 || strcmp(bbuf, "dp9ik") == 0){
			proto = bbuf;
			if(strcmp(proto, "dp9ik") == 0){
				dp9ik = 1;
				break;
			}
		}
		bbuf = p;
	}
	if(proto == nil)
		sysfatal("server did not offer p9sk1 or dp9ik");
	proto = estrdup(proto);
	sprint(buf2, "%s %s", proto, dom);
	if(write(fd, buf2, strlen(buf2)+1) != strlen(buf2)+1)
		sysfatal("cannot write user/domain choice in p9any");
	if(v2){
		if(readstr(fd, buf, sizeof buf) < 0)
			sysfatal("cannot read OK in p9any: %r");
		if(memcmp(buf, "OK\0", 3) != 0)
			sysfatal("did not get OK in p9any: got %s", buf);
	}
	genrandom(crand, 2*NONCELEN);
	genrandom(cchal, CHALLEN);
	if(write(fd, cchal, CHALLEN) != CHALLEN)
		sysfatal("cannot write p9sk1 challenge: %r");

	n = TICKREQLEN;
	if(dp9ik)
		n += PAKYLEN;

	if(readn(fd, trbuf, n) != n || convM2TR(trbuf, TICKREQLEN, &tr) <= 0)
		sysfatal("cannot read ticket request in p9sk1: %r");

	if(!findkey(&authkey, user, tr.authdom, proto)){
again:		if(!getkey(&authkey, user, tr.authdom, proto, pass))
			sysfatal("no password");
	}

	strecpy(tr.hostid, tr.hostid+sizeof tr.hostid, user);
	strecpy(tr.uid, tr.uid+sizeof tr.uid, user);

	if(dp9ik){
		memmove(y, trbuf+TICKREQLEN, PAKYLEN);
		n = gettickets(&authkey, &tr, y, tbuf, sizeof(tbuf));
	} else {
		n = gettickets(&authkey, &tr, nil, tbuf, sizeof(tbuf));
	}
	if(n <= 0)
		sysfatal("cannot get auth tickets in p9sk1: %r");

	m = convM2T(tbuf, n, &t, &authkey);
	if(m <= 0 || t.num != AuthTc){
		print("?password mismatch with auth server\n");
		if(pass != nil && *pass)
			sysfatal("wrong password");
		goto again;
	}
	n -= m;
	memmove(tbuf, tbuf+m, n);

	if(dp9ik && write(fd, y, PAKYLEN) != PAKYLEN)
		sysfatal("cannot send authpak public key back: %r");

	auth.num = AuthAc;
	memmove(auth.rand, crand, NONCELEN);
	memmove(auth.chal, tr.chal, CHALLEN);
	m = convA2M(&auth, tbuf+n, sizeof(tbuf)-n, &t);
	n += m;

	if(write(fd, tbuf, n) != n)
		sysfatal("cannot send ticket and authenticator back: %r");

	if((n=readn(fd, tbuf, m)) != m || memcmp(tbuf, "cpu:", 4) == 0){
		if(n <= 4)
			sysfatal("cannot read authenticator");

		/*
		 * didn't send back authenticator:
		 * sent back fatal error message.
		 */
		memmove(buf, tbuf, n);
		i = readn(fd, buf+n, sizeof buf-n-1);
		if(i > 0)
			n += i;
		buf[n] = 0;
		sysfatal("server says: %s", buf);
	}
	
	if(convM2A(tbuf, n, &auth, &t) <= 0
	|| auth.num != AuthAs || tsmemcmp(auth.chal, cchal, CHALLEN) != 0){
		print("?you and auth server agree about password.\n");
		print("?server is confused.\n");
		sysfatal("server lies");
	}
	memmove(crand+NONCELEN, auth.rand, NONCELEN);

	// print("i am %s there.\n", t.suid);

	ai = mallocz(sizeof(AuthInfo), 1);
	ai->suid = estrdup(t.suid);
	ai->cuid = estrdup(t.cuid);
	if(dp9ik){
		static char info[] = "Plan 9 session secret";
		ai->nsecret = 256;
		ai->secret = mallocz(ai->nsecret, 1);
		hkdf_x(	crand, 2*NONCELEN,
			(uchar*)info, sizeof(info)-1,
			(uchar*)t.key, NONCELEN,
			ai->secret, ai->nsecret,
			hmac_sha2_256, SHA2_256dlen);
	} else {
		ai->nsecret = 8;
		ai->secret = mallocz(ai->nsecret, 1);
		des56to64((uchar*)t.key, ai->secret);
	}

	memset(&t, 0, sizeof(t));
	memset(&auth, 0, sizeof(auth));
	memset(&authkey, 0, sizeof(authkey));
	memset(cchal, 0, sizeof(cchal));
	memset(crand, 0, sizeof(crand));
	free(proto);

	return ai;
}

static int
findkey(Authkey *key, char *user, char *dom, char *proto)
{
	char buf[8192], *f[50], *p, *ep, *nextp, *hex, *pass, *id, *role;
	int nf, haveproto,  havedom, i;

	for(p=secstorebuf; *p; p=nextp){
		nextp = strchr(p, '\n');
		if(nextp == nil){
			ep = p+strlen(p);
			nextp = "";
		}else{
			ep = nextp++;
		}
		if(ep-p >= sizeof buf){
			print("warning: skipping long line in secstore factotum file\n");
			continue;
		}
		memmove(buf, p, ep-p);
		buf[ep-p] = 0;
		nf = tokenize(buf, f, nelem(f));
		if(nf == 0 || strcmp(f[0], "key") != 0)
			continue;
		id = pass = hex = role = nil;
		havedom = haveproto = 0;
		for(i=1; i<nf; i++){
			if(strncmp(f[i], "user=", 5) == 0)
				id = f[i]+5;
			if(strncmp(f[i], "!password=", 10) == 0)
				pass = f[i]+10;
			if(strncmp(f[i], "!hex=", 5) == 0)
				hex = f[i]+5;
			if(strncmp(f[i], "role=", 5) == 0)
				role = f[i]+5;
			if(strncmp(f[i], "dom=", 4) == 0 && strcmp(f[i]+4, dom) == 0)
				havedom = 1;
			if(strncmp(f[i], "proto=", 6) == 0 && strcmp(f[i]+6, proto) == 0)
				haveproto = 1;
		}
		if(!haveproto || !havedom)
			continue;
		if(role != nil && strcmp(role, "client") != 0)
			continue;
		if(id == nil || strcmp(user, id) != 0)
			continue;
		if(pass == nil && hex == nil)
			continue;
		if(hex != nil){
			memset(key, 0, sizeof(*key));
			if(strcmp(proto, "dp9ik") == 0) {
				if(dec16(key->aes, AESKEYLEN, hex, strlen(hex)) != AESKEYLEN)
					continue;
			} else {
				if(dec16((uchar*)key->des, DESKEYLEN, hex, strlen(hex)) != DESKEYLEN)
					continue;
			}
		} else {
			passtokey(key, pass);
		}
		if(strcmp(proto, "dp9ik") == 0)
			authpak_hash(key, user);
		memset(buf, 0, sizeof buf);
		return 1;
	}
	memset(buf, 0, sizeof buf);
	return 0;
}

static int
getkey(Authkey *key, char *user, char *dom, char *proto, char *pass)
{
	char buf[1024];

	if(pass != nil && *pass)
		pass = estrdup(pass);
	else {
		snprint(buf, sizeof buf, "%s@%s %s password", user, dom, proto);
		pass = readcons(buf, nil, 1);
		memset(buf, 0, sizeof buf);
	}
	if(pass != nil){
		snprint(secstorebuf, sizeof(secstorebuf), "key proto=%q dom=%q user=%q !password=%q\n",
			proto, dom, user, pass);
		memset(pass, 0, strlen(pass));
		free(pass);
		return findkey(key, user, dom, proto);
	}
	return 0;
}
