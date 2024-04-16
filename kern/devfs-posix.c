#include	"u.h"
#include	<sys/types.h>
#include	<sys/time.h>
#include	<sys/stat.h>
#include	<dirent.h>
#include	<fcntl.h>
#include	<errno.h>
#include	<stdio.h> /* for remove, rename */
#include	<limits.h>

#ifndef NAME_MAX
#	define NAME_MAX 256
#endif
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

typedef	struct Ufsinfo	Ufsinfo;
struct Ufsinfo
{
	int	mode;
	int	fd;
	int	uid;
	int	gid;
	DIR*	dir;
	vlong	offset;
	QLock	oq;
	char*	path;
	char nextname[NAME_MAX];
};

static	Qid	fsqid(struct stat *);
static	char*	catpath(char*, char*);
static	ulong	fsdirread(Chan*, uchar*, int, ulong);
static	int	fsomode(int);

static char*
lastelem(char *s)
{
	char *t;

	if((t = strrchr(s, '/')) == nil)
		return s;
	if(t[1] == 0)
		return t;
	return t+1;
}
	
static Chan*
fsattach(char *spec)
{
	Chan *c;
	struct stat stbuf;
	static int devno;
	Ufsinfo *uif;

	if(stat("/", &stbuf) < 0)
		error(strerror(errno));

	c = devattach('U', spec);

	uif = mallocz(sizeof(Ufsinfo), 1);
	uif->mode = stbuf.st_mode;
	uif->uid = stbuf.st_uid;
	uif->gid = stbuf.st_gid;
	uif->path = strdup("/");

	c->aux = uif;
	c->dev = devno++;
	c->qid.type = QTDIR;
/*print("fsattach %s\n", chanpath(c));*/

	return c;
}

static Chan*
fsclone(Chan *c, Chan *nc)
{
	Ufsinfo *uif;

	uif = mallocz(sizeof(Ufsinfo), 1);
	*uif = *(Ufsinfo*)c->aux;
	uif->path = strdup(uif->path);
	nc->aux = uif;

	return nc;
}

static int
fswalk1(Chan *c, char *name)
{
	struct stat stbuf;
	Ufsinfo *uif;
	char *path;

	/*print("** fs walk '%s' -> %s\n", path, name);  */

	uif = c->aux;
	path = catpath(uif->path, name);
	if(stat(path, &stbuf) < 0){
		free(path);
		return 0;
	}
	free(uif->path);
	uif->path = path;

	uif->mode = stbuf.st_mode;
	uif->uid = stbuf.st_uid;
	uif->gid = stbuf.st_gid;

	c->qid = fsqid(&stbuf);

	return 1;
}

static Walkqid*
fswalk(Chan *c, Chan *nc, char **name, int nname)
{
	int i;
	Walkqid *wq;

	if(nc != nil)
		panic("fswalk: nc != nil");
	wq = smalloc(sizeof(Walkqid)+(nname-1)*sizeof(Qid));
	nc = devclone(c);
	fsclone(c, nc);
	wq->clone = nc;
	for(i=0; i<nname; i++){
		if(fswalk1(nc, name[i]) == 0)
			break;
		wq->qid[i] = nc->qid;
	}
	if(i != nname){
		cclose(nc);
		wq->clone = nil;
	}
	wq->nqid = i;
	return wq;
}
	
static int
fsstat(Chan *c, uchar *buf, int n)
{
	Dir d;
	struct stat stbuf;
	Ufsinfo *uif;

	if(n < BIT16SZ)
		error(Eshortstat);

	uif = c->aux;
	if(stat(uif->path, &stbuf) < 0)
		error(strerror(errno));

	d.name = lastelem(uif->path);
	d.uid = eve;
	d.gid = eve;
	d.muid = eve;
	d.qid = c->qid;
	d.mode = (c->qid.type<<24)|(stbuf.st_mode&0777);
	d.atime = stbuf.st_atime;
	d.mtime = stbuf.st_mtime;
	d.length = stbuf.st_size;
	d.type = 'U';
	d.dev = c->dev;
	return convD2M(&d, buf, n);
}

static Chan*
fsopen(Chan *c, int mode)
{
	Ufsinfo *uif;
	int omode;

	omode = openmode(mode);

	uif = c->aux;
	if(c->qid.type & QTDIR) {
		if(omode != OREAD)
			error(Eperm);
		if((uif->dir = opendir(uif->path)) == NULL)
			error(strerror(errno));
	}	
	else {
		int m = fsomode(omode & 3);
		if(mode & OTRUNC)
			m |= O_TRUNC;
		if((uif->fd = open(uif->path, m, 0666)) < 0)
			error(strerror(errno));
	}
	uif->offset = 0;

	c->offset = 0;
	c->mode = omode;
	c->flag |= COPEN;
	return c;
}

static Chan*
fscreate(Chan *c, char *name, int mode, ulong perm)
{
	struct stat stbuf;
	Ufsinfo *uif;
	char *path;
	DIR *dir;
	int fd, omode;

	omode = openmode(mode & ~OEXCL);

	uif = c->aux;
	path = catpath(uif->path, name);
	if(waserror()){
		free(path);
		nexterror();
	}
	fd = -1;
	dir = NULL;
	if(perm & DMDIR) {
		if(omode != OREAD)
			error(Eperm);
		if(mkdir(path, uif->mode & perm & 0777) < 0){
			if(errno != EEXIST || mode & OEXCL)
				error(strerror(errno));
		}
		if((dir = opendir(path)) == NULL)
			error(strerror(errno));
		if(waserror()){
			closedir(dir);
			nexterror();
		}
	}
	else {
		int m = omode & 3;
		/*
		 * POSIX specifies that O_TRUNC with O_RDONLY is undefined.
		 * OpenBSD chooses to return an error.
		 */
		m = fsomode(m == OREAD ? ORDWR : m) | O_CREAT | O_TRUNC;
		if(mode & OEXCL)
			m |= O_EXCL;
		if((fd = open(path, m, uif->mode & perm & 0777)) < 0)
			error(strerror(errno));
		if(waserror()){
			close(fd);
			nexterror();
		}
	}
	if(stat(path, &stbuf) < 0)
		error(strerror(errno));
	c->qid = fsqid(&stbuf);
	uif->fd = fd;
	uif->dir = dir;
	poperror();

	free(uif->path);
	uif->path = path;
	uif->offset = 0;
	poperror();

	c->offset = 0;
	c->mode = omode;
	c->flag |= COPEN;
	return c;
}

static void
fsclose(Chan *c)
{
	Ufsinfo *uif;

	uif = c->aux;
	if(c->flag & COPEN) {
		if(c->qid.type & QTDIR)
			closedir(uif->dir);
		else
			close(uif->fd);
		c->flag &= ~COPEN;
		if(c->flag & CRCLOSE) {
			devtab[c->type]->remove(c);
			return;
		}
	}
	free(uif->path);
	free(uif);
}

static long
fsread(Chan *c, void *va, long n, vlong offset)
{
	int fd;
	vlong r;
	Ufsinfo *uif;

/*print("fsread %s\n", chanpath(c));*/
	if(c->qid.type & QTDIR)
		return fsdirread(c, va, n, offset);

	uif = c->aux;
	qlock(&uif->oq);
	if(waserror()) {
		qunlock(&uif->oq);
		nexterror();
	}
	fd = uif->fd;
	if(uif->offset != offset) {
		r = lseek(fd, offset, 0);
		if(r < 0)
			error(strerror(errno));
		uif->offset = offset;
	}

	n = read(fd, va, n);
	if(n < 0)
		error(strerror(errno));

	uif->offset += n;
	qunlock(&uif->oq);
	poperror();

	return n;
}

static long
fswrite(Chan *c, void *va, long n, vlong offset)
{
	int fd;
	vlong r;
	Ufsinfo *uif;

	uif = c->aux;
	qlock(&uif->oq);
	if(waserror()) {
		qunlock(&uif->oq);
		nexterror();
	}
	fd = uif->fd;
	if(uif->offset != offset) {
		r = lseek(fd, offset, 0);
		if(r < 0)
			error(strerror(errno));
		uif->offset = offset;
	}

	n = write(fd, va, n);
	if(n < 0)
		error(strerror(errno));

	uif->offset += n;
	qunlock(&uif->oq);
	poperror();

	return n;
}

static void
fsremove(Chan *c)
{
	int n;
	Ufsinfo *uif;

	if(waserror()){
		fsclose(c);
		nexterror();
	}
	uif = c->aux;
	if(c->qid.type & QTDIR)
		n = rmdir(uif->path);
	else
		n = remove(uif->path);
	if(n < 0)
		error(strerror(errno));
	poperror();
	fsclose(c);
}

int
fswstat(Chan *c, uchar *buf, int n)
{
	Dir d;
	struct stat stbuf;
	char strs[NAME_MAX*3];
	Ufsinfo *uif;

	/*
	 * wstat is supposed to be atomic.
	 * we check all the things we can before trying anything.
	 * still, if we are told to truncate a file and rename it and only
	 * one works, we're screwed.  in such cases we leave things
	 * half broken and return an error.  it's hardly perfect.
	 */
	if(convM2D(buf, n, &d, strs) != n)
		error(Ebadstat);
	
	uif = c->aux;
	if(stat(uif->path, &stbuf) < 0)
		error(strerror(errno));

	/*
	 * try things in increasing order of harm to the file.
	 * mtime should come after truncate so that if you
	 * do both the mtime actually takes effect, but i'd rather
	 * leave truncate until last.
	 * (see above comment about atomicity).
	 */
	if(~d.mode != 0 && (int)(d.mode&0777) != (int)(stbuf.st_mode&0777)) {
		if(chmod(uif->path, d.mode&0777) < 0)
			error(strerror(errno));
		uif->mode &= ~0777;
		uif->mode |= d.mode&0777;
	}
	if(~d.atime != 0 || ~d.mtime != 0){
		struct timeval t[2];
		t[0].tv_sec = ~d.atime != 0 ? d.atime : stbuf.st_atime;
		t[0].tv_usec = 0;
		t[1].tv_sec = ~d.mtime != 0 ? d.mtime : stbuf.st_mtime;
		t[1].tv_usec = 0;
		utimes(uif->path, t);
	}
	if(d.name[0] && strcmp(d.name, lastelem(uif->path)) != 0) {
		char *base, *newpath;

		base = strdup(uif->path);
		*lastelem(base) = 0;
		newpath = catpath(base, d.name);
		free(base);
		if(waserror()){
			free(newpath);
			nexterror();
		}
		if(stat(newpath, &stbuf) >= 0)
			error(Eexist);
		if(rename(uif->path, newpath) < 0)
			error(strerror(errno));
		free(uif->path);
		uif->path = newpath;
		poperror();
	}

/*
	p = name2pass(gid, d.gid);
	if(p == 0)
		error(Eunknown);

	if(p->id != stbuf.st_gid) {
		if(chown(old, stbuf.st_uid, p->id) < 0)
			error(strerror(errno));

		uif->gid = p->id;
	}
*/

	if((uvlong)d.length != (uvlong)~0 && truncate(uif->path, d.length) < 0)
		error(strerror(errno));

	return n;
}

static Qid
fsqid(struct stat *st)
{
	Qid q;

	q.type = 0;
	if((st->st_mode&S_IFMT) ==  S_IFDIR)
		q.type = QTDIR;

	q.path = (uvlong)st->st_dev<<32 | st->st_ino;
	q.vers = st->st_mtime;

	return q;
}

static char*
catpath(char *base, char *ext)
{
	char *path;
	int n, m;

	n = strlen(base);
	m = strlen(ext);
	path = malloc(n+m+2);
	memmove(path, base, n);
	if(n > 0 && path[n-1] != '/')
		path[n++] = '/';
	memmove(path+n, ext, m+1);
	return path;
}

static int
isdots(char *name)
{
	if(name[0] != '.')
		return 0;
	if(name[1] == '\0')
		return 1;
	if(name[1] != '.')
		return 0;
	if(name[2] == '\0')
		return 1;
	return 0;
}

static int
p9readdir(char *name, Ufsinfo *uif)
{
	struct dirent *de;
	
	if(uif->nextname[0]){
		strcpy(name, uif->nextname);
		uif->nextname[0] = 0;
		return 1;
	}

	de = readdir(uif->dir);
	if(de == NULL)
		return 0;
		
	strcpy(name, de->d_name);
	return 1;
}

static ulong
fsdirread(Chan *c, uchar *va, int count, ulong offset)
{
	int i;
	Dir d;
	long n;
	char de[NAME_MAX];
	struct stat stbuf;
	Ufsinfo *uif;

/*print("fsdirread %s\n", chanpath(c));*/
	i = 0;
	uif = c->aux;

	errno = 0;
	if(uif->offset != offset) {
		if(offset != 0)
			error("bad offset in fsdirread");
		uif->offset = offset;  /* sync offset */
		uif->nextname[0] = 0;
		rewinddir(uif->dir);
	}

	while(i+BIT16SZ < count) {
		char *p;

		if(!p9readdir(de, uif))
			break;

		if(de[0]==0 || isdots(de))
			continue;

		d.name = de;
		p = catpath(uif->path, de);
		if(stat(p, &stbuf) < 0) {
			/* fprint(2, "dir: bad path %s\n", path); */
			/* but continue... probably a bad symlink */
			memset(&stbuf, 0, sizeof stbuf);
		}
		free(p);

		d.uid = eve;
		d.gid = eve;
		d.muid = eve;
		d.qid = fsqid(&stbuf);
		d.mode = (d.qid.type<<24)|(stbuf.st_mode&0777);
		d.atime = stbuf.st_atime;
		d.mtime = stbuf.st_mtime;
		d.length = stbuf.st_size;
		d.type = 'U';
		d.dev = c->dev;
		n = convD2M(&d, (uchar*)va+i, count-i);
		if(n == BIT16SZ){
			strcpy(uif->nextname, de);
			break;
		}
		i += n;
	}
/*print("got %d\n", i);*/
	uif->offset += i;
	return i;
}

static int
fsomode(int m)
{
	switch(m) {
	case OREAD:
	case OEXEC:
		return O_RDONLY;
	case OWRITE:
		return O_WRONLY;
	case ORDWR:
		return O_RDWR;
	}
	error(Ebadarg);
	return 0;
}

Dev fsdevtab = {
	'U',
	"fs",

	devreset,
	devinit,
	devshutdown,
	fsattach,
	fswalk,
	fsstat,
	fsopen,
	fscreate,
	fsclose,
	fsread,
	devbread,
	fswrite,
	devbwrite,
	fsremove,
	fswstat,
};
