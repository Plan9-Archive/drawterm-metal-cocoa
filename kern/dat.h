#define	KNAMELEN		28	/* max length of name held in kernel */

#define	BLOCKALIGN		8

typedef struct Block	Block;
typedef struct Chan	Chan;
typedef struct Cmdbuf	Cmdbuf;
typedef struct Cmdtab	Cmdtab;
typedef struct Conf	Conf;
typedef struct Dev	Dev;
typedef struct Dirtab	Dirtab;
typedef struct Egrp	Egrp;
typedef struct Evalue	Evalue;
typedef struct Fgrp	Fgrp;
typedef struct DevConf	DevConf;
typedef struct Label	Label;
typedef struct Log	Log;
typedef struct Logflag	Logflag;
typedef struct Mount	Mount;
typedef struct Mntrpc	Mntrpc;
typedef struct Mntwalk	Mntwalk;
typedef struct Mnt	Mnt;
typedef struct Mhead	Mhead;
typedef struct Path	Path;
typedef struct Pgrps	Pgrps;
typedef struct Pgrp	Pgrp;
typedef struct Proc	Proc;
typedef struct Queue	Queue;
typedef struct Ref	Ref;
typedef struct Rendez	Rendez;
typedef struct Rgrp	Rgrp;
typedef struct RWlock	RWlock;
typedef struct Waitq	Waitq;
typedef struct Walkqid	Walkqid;
typedef struct Kmesg	Kmesg;
typedef int    Devgen(Chan*, char*, Dirtab*, int, int, Dir*);

#include "fcall.h"

enum
{
	SnarfSize = 64*1024,
};

struct Conf
{
	ulong	nmach;		/* processors */
	ulong	nproc;		/* processes */
	ulong	pipeqsize;	/* size in bytes of pipe queues */
};

struct Label
{
	jmp_buf	buf;
};

struct Ref
{
	Lock lk;
	long	ref;
};

struct Rendez
{
	Lock lk;
	Proc	*p;
};

struct RWlock	/* changed from kernel */
{
	int	readers;
	Lock	lk;
	QLock	x;
	QLock	k;
};

struct Talarm
{
	Lock lk;
	Proc	*list;
};

struct Alarms
{
	QLock lk;
	Proc	*head;
};

/*
 * Access types in namec & channel flags
 */
enum
{
	Aaccess,			/* as in stat, wstat */
	Abind,			/* for left-hand-side of bind */
	Atodir,				/* as in chdir */
	Aopen,				/* for i/o */
	Amount,				/* to be mounted or mounted upon */
	Acreate,			/* is to be created */
	Aremove,			/* will be removed by caller */

	COPEN	= 0x0001,		/* for i/o */
	CMSG	= 0x0002,		/* the message channel for a mount */
/*	CCREATE	= 0x0004,		permits creation if c->mnt */
	CCEXEC	= 0x0008,		/* close on exec */
	CFREE	= 0x0010,		/* not in use */
	CRCLOSE	= 0x0020,		/* remove on close */
	CCACHE	= 0x0080,		/* client cache */
};

/* flag values */
enum
{
	BINTR	=	(1<<0),
	BFREE	=	(1<<1),
	Bipck	=	(1<<2),		/* ip checksum */
	Budpck	=	(1<<3),		/* udp checksum */
	Btcpck	=	(1<<4),		/* tcp checksum */
	Bpktck	=	(1<<5),		/* packet checksum */
};

struct Block
{
	Block*	next;
	Block*	list;
	uchar*	rp;			/* first unconsumed byte */
	uchar*	wp;			/* first empty byte */
	uchar*	lim;			/* 1 past the end of the buffer */
	uchar*	base;			/* start of the buffer */
	void	(*free)(Block*);
	ushort	flag;
	ushort	checksum;		/* IP checksum of complete packet (minus media header) */
};
#define BLEN(s)	((s)->wp - (s)->rp)
#define BALLOC(s) ((s)->lim - (s)->base)

struct Chan
{
	Ref	ref;
	Lock	lk;
	Chan*	next;			/* allocation */
	Chan*	link;
	vlong	offset;			/* in fd */
	vlong	devoffset;		/* in underlying device; see read */
	ushort	type;
	ulong	dev;
	ushort	mode;			/* read/write */
	ushort	flag;
	Qid	qid;
	int	fid;			/* for devmnt */
	ulong	iounit;			/* chunk size for i/o; 0==default */
	Mhead*	umh;			/* mount point that derived Chan; used in unionread */
	Chan*	umc;			/* channel in union; held for union read */
	QLock	umqlock;		/* serialize unionreads */
	int	uri;			/* union read index */
	int	dri;			/* devdirread index */
	uchar*	dirrock;		/* directory entry rock for translations */
	int	nrock;
	int	mrock;
	QLock	rockqlock;
	int	ismtpt;
	Mnt*	mux;			/* Mnt for clients using me for messages */
	union {
		void*	aux;
		ulong	mid;		/* for ns in devproc */
	};
	Chan*	mchan;			/* channel to mounted server */
	Qid	mqid;			/* qid of root of mount point */
	Path*	path;
};

struct Path
{
	Ref	ref;
	char	*s;
	Chan	**mtpt;			/* mtpt history */
	int	len;			/* strlen(s) */
	int	alen;			/* allocated length of s */
	int	mlen;			/* number of path elements */
	int	malen;			/* allocated length of mtpt */
};

struct Dev
{
	int	dc;
	char*	name;

	void	(*reset)(void);
	void	(*init)(void);
	void	(*shutdown)(void);
	Chan*	(*attach)(char*);
	Walkqid*	(*walk)(Chan*, Chan*, char**, int);
	int	(*stat)(Chan*, uchar*, int);
	Chan*	(*open)(Chan*, int);
	Chan*	(*create)(Chan*, char*, int, ulong);
	void	(*close)(Chan*);
	long	(*read)(Chan*, void*, long, vlong);
	Block*	(*bread)(Chan*, long, ulong);
	long	(*write)(Chan*, void*, long, vlong);
	long	(*bwrite)(Chan*, Block*, ulong);
	void	(*remove)(Chan*);
	int	(*wstat)(Chan*, uchar*, int);
	void	(*power)(int);	/* power mgt: power(1) => on, power (0) => off */
	int	(*config)(int, char*, DevConf*);	// returns nil on error
};

struct Dirtab
{
	char	name[KNAMELEN];
	Qid	qid;
	vlong length;
	long	perm;
};

struct Walkqid
{
	Chan	*clone;
	int	nqid;
	Qid	qid[1];
};

enum
{
	NSMAX	=	1000,
	NSLOG	=	7,
	NSCACHE	=	(1<<NSLOG),
};

struct Mntwalk				/* state for /proc/#/ns */
{
	int		cddone;
	ulong	id;
	Mhead*	mh;
	Mount*	cm;
};

struct Mount
{
	ulong	mountid;
	Mount*	next;
	Mhead*	head;
	Mount*	copy;
	Mount*	order;
	Chan*	to;			/* channel replacing channel */
	int	mflag;
	char	*spec;
};

struct Mhead
{
	Ref ref;
	RWlock	lock;
	Chan*	from;			/* channel mounted upon */
	Mount*	mount;			/* what's mounted upon it */
	Mhead*	hash;			/* Hash chain */
};

struct Mnt
{
	Lock lk;
	/* references are counted using c->ref; channels on this mount point incref(c->mchan) == Mnt.c */
	Chan	*c;		/* Channel to file service */
	Proc	*rip;		/* Reader in progress */
	Mntrpc	*queue;		/* Queue of pending requests on this channel */
	ulong	id;		/* Multiplexer id for channel check */
	Mnt	*list;		/* Free list */
	int	flags;		/* cache */
	int	msize;		/* data + IOHDRSZ */
	char	*version;			/* 9P version */
	Queue	*q;		/* input queue */
};

enum
{
	NUser,				/* note provided externally */
	NExit,				/* deliver note quietly */
	NDebug,				/* print debug message */
};

enum
{
	RENDLOG	=	5,
	RENDHASH =	1<<RENDLOG,		/* Hash to lookup rendezvous tags */
	MNTLOG	=	5,
	MNTHASH =	1<<MNTLOG,		/* Hash to walk mount table */
	NFD =		100,		/* per process file descriptors */
};
#define REND(p,s)	((p)->rendhash[(s)&((1<<RENDLOG)-1)])
#define MOUNTH(p,qid)	((p)->mnthash[(qid).path&((1<<MNTLOG)-1)])

struct Pgrp
{
	Ref ref;				/* also used as a lock when mounting */
	int	noattach;
	QLock	debug;			/* single access via devproc.c */
	RWlock	ns;			/* Namespace n read/one write lock */
	Mhead	*mnthash[MNTHASH];
};

struct Rgrp
{
	Ref ref;
	Proc	*rendhash[RENDHASH];	/* Rendezvous tag hash */
};

struct Egrp
{
	Ref ref;
	RWlock lk;
	Evalue	**ent;
	int nent;
	int ment;
	ulong	path;	/* qid.path of next Evalue to be allocated */
	ulong	vers;	/* of Egrp */
};

struct Evalue
{
	char	*name;
	char	*value;
	int	len;
	Evalue	*link;
	Qid	qid;
};

struct Fgrp
{
	Ref	ref;
	Chan	**fd;
	int	nfd;			/* number allocated */
	int	maxfd;			/* highest fd in use */
	int	exceed;			/* debugging */
};

enum
{
	DELTAFD	= 20,		/* incremental increase in Fgrp.fd's */
	NERR = 20
};

typedef uvlong	Ticks;

enum
{
	Running,
	Rendezvous,
	Wakeme,
};

struct Proc
{
	uint	state;
	uint	mach;

	ulong	pid;

	Pgrp	*pgrp;		/* Process group for namespace */
	Fgrp	*fgrp;		/* File descriptor group */
	Rgrp	*rgrp;

	Lock	rlock;		/* sync sleep/wakeup with postnote */
	Rendez	*r;		/* rendezvous point slept on */
	Rendez	sleep;		/* place for syssleep/debug */
	int	notepending;	/* note issued but not acted on */
	int	kp;		/* true if a kernel process */

	void*	rendtag;	/* Tag for rendezvous */
	void*	rendval;	/* Value for rendezvous */
	Proc	*rendhash;	/* Hash list for tag values */

	int	nerrlab;
	Label	errlab[NERR];
	char	user[KNAMELEN];
	char	*syserrstr;	/* last error from a system call, errbuf0 or 1 */
	char	*errstr;	/* reason we're unwinding the error stack, errbuf1 or 0 */
	char	errbuf0[ERRMAX];
	char	errbuf1[ERRMAX];
	char	genbuf[128];	/* buffer used e.g. for last name element from namec */
	char	text[KNAMELEN];

	Chan	*slash;
	Chan	*dot;

	Proc	*qnext;

	void	(*fn)(void*);
	void	*arg;

	char oproc[1024];	/* reserved for os */

};

enum
{
	PRINTSIZE =	256,
	MAXCRYPT = 	127,
	NUMSIZE	=	12,		/* size of formatted number */
	MB =		(1024*1024),
	READSTR =	1000,		/* temporary buffer size for device reads */
};

extern	int	cpuserver;
extern	Dev*	devtab[];
extern  char	*eve;
extern	char	hostdomain[];
extern  Queue*	kbdq;
extern  Queue*	kprintoq;
extern	char*	statename[];
extern	char	*sysname;
extern	uint	qiomaxatomic;
extern	Conf	conf;

/*
 *  action log
 */
struct Log {
	Lock lk;
	int	opens;
	char*	buf;
	char	*end;
	char	*rptr;
	int	len;
	int	nlog;
	int	minread;

	int	logmask;	/* mask of things to debug */

	QLock	readq;
	Rendez	readr;
};

struct Logflag {
	char*	name;
	int	mask;
};

enum
{
	NCMDFIELD = 128
};

struct Cmdbuf
{
	char	*buf;
	char	**f;
	int	nf;
};

struct Cmdtab
{
	int	index;	/* used by client to switch on result */
	char	*cmd;	/* command name */
	int	narg;	/* expected #args; 0 ==> variadic */
};

/* queue state bits,  Qmsg, Qcoalesce, and Qkick can be set in qopen */
enum
{
	/* Queue.state */
	Qstarve		= (1<<0),	/* consumer starved */
	Qmsg		= (1<<1),	/* message stream */
	Qclosed		= (1<<2),	/* queue has been closed/hungup */
	Qflow		= (1<<3),	/* producer flow controlled */
	Qcoalesce	= (1<<4),	/* coallesce packets on read */
	Qkick		= (1<<5),	/* always call the kick routine after qwrite */
};

#define DEVDOTDOT -1

extern Proc	*_getproc(void);
extern void	_setproc(Proc*);
#define	up	(_getproc())

/*
 * Log console output so it can be retrieved via /dev/kmesg.
 * This is good for catching boot-time messages after the fact.
 */
struct Kmesg {
	Lock lk;
	uint n;
	char buf[16384];
};

extern Kmesg kmesg;
