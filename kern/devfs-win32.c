#include	<windows.h>
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<fcntl.h>

#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

#include	<libsec.h>	/* for sha1 in pathhash() */

typedef struct DIR	DIR;
typedef	struct Ufsinfo	Ufsinfo;

enum
{
	TPATH_ROOT	= 0,	// ""
	TPATH_VOLUME	= 1,	// "C:"
	TPATH_FILE	= 2,	// "C:\bla"
};

struct DIR
{
	// for FindFileFirst()
	HANDLE		handle;
	WIN32_FIND_DATA	wfd;

	// for GetLogicalDriveStrings()
	wchar_t		*drivebuf;
	wchar_t		*drivep;

	// dont move to the next item
	int		keep;
};

struct Ufsinfo
{
	int	mode;
	HANDLE	fh;
	DIR*	dir;
	vlong	offset;
	QLock	oq;
	wchar_t	*path;
};

static	wchar_t *catpath(wchar_t *, char *, wchar_t *);
static	ulong	fsdirread(Chan*, uchar*, int, vlong);
static	ulong	fsaccess(int);
static	ulong	pathtype(wchar_t *);
static	int	checkvolume(wchar_t *);

static ulong
unixtime(FILETIME *ft)
{
	vlong t;
	t = ((vlong)ft->dwHighDateTime << 32)|((vlong)ft->dwLowDateTime);
	t -= 116444736000000000LL;
	return ((t<0)?(-1 - (-t - 1)) : t)/10000000;
}

static FILETIME
filetime(ulong ut)
{
	FILETIME ft;
	vlong t = (vlong)ut * 10000000LL;
	t += 116444736000000000LL;
	ft.dwLowDateTime = t;
	ft.dwHighDateTime = t >> 32;
	return ft;
}

static uvlong
pathhash(wchar_t *p)
{
	uchar digest[SHA1dlen];
	sha1((uchar*)p, wcslen(p)*sizeof(wchar_t), digest, nil);
	return *(uvlong*)digest;
}

static ulong
wfdtodmode(WIN32_FIND_DATA *wfd)
{
	int m;
	m = DMREAD|DMWRITE|DMEXEC;
	if(wfd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		m |= DMDIR;
	if(wfd->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
		m &= ~DMWRITE;
	m |= (m & 07)<<3;
	m |= (m & 07)<<6;
	return m;
}

static Qid
wfdtoqid(wchar_t *path, WIN32_FIND_DATA *wfd)
{
	ulong t;
	WIN32_FIND_DATA f;
	Qid q;

	t = pathtype(path);
	switch(t){
	case TPATH_VOLUME:
	case TPATH_ROOT:
		q.type = QTDIR;
		q.path = pathhash(path);
		q.vers = 0;
		break;

	case TPATH_FILE:
		if(!wfd){
			HANDLE h;
			if((h = FindFirstFile(path, &f))==INVALID_HANDLE_VALUE)
				oserror();
			FindClose(h);
			wfd = &f;
		}
		q.type = (wfd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? QTDIR : QTFILE;
		q.path = pathhash(path);
		q.vers = unixtime(&wfd->ftLastWriteTime);
		break;
	}
	return q;
}

static void
wfdtodir(wchar_t *path, Dir *d, WIN32_FIND_DATA *wfd)
{
	extern ulong kerndate;
	WIN32_FIND_DATA f;

	switch(pathtype(path)){
	case TPATH_VOLUME:
	case TPATH_ROOT:
		wfd = nil;
		d->mode = 0777 | DMDIR;
		d->atime = seconds();
		d->mtime = kerndate;
		d->length = 0;
		break;

	case TPATH_FILE:
		if(wfd == nil){
			HANDLE h;
			if((h = FindFirstFile(path, &f))==INVALID_HANDLE_VALUE)
				oserror();
			FindClose(h);
			wfd = &f;
		}
		d->mode		= wfdtodmode(wfd);
		d->atime	= unixtime(&wfd->ftLastAccessTime);
		d->mtime	= unixtime(&wfd->ftLastWriteTime);
		d->length	= ((uvlong)wfd->nFileSizeHigh << 32)|((uvlong)wfd->nFileSizeLow);
		break;
	}
	d->qid = wfdtoqid(path, wfd);
	d->uid = eve;
	d->gid = eve;
	d->muid = eve;
}

static char*
lastelem(Chan *c)
{
	char *s, *t;

	s = chanpath(c);
	if((t = strrchr(s, '/')) == nil)
		return s;
	if(t[1] == 0)
		return t;
	return t+1;
}

static ulong
pathtype(wchar_t *path)
{
	if(path[0] == 0 || path[1] == 0)
		return TPATH_ROOT;
	if(path[1] == ':' && path[2] == 0)
		return TPATH_VOLUME;
	return TPATH_FILE;
}

static int
checkvolume(wchar_t *path)
{
	wchar_t vol[MAX_PATH];
	wchar_t volname[MAX_PATH];
	wchar_t fsysname[MAX_PATH];
	DWORD complen;
	DWORD flags;

	wcscpy(vol, path);
	wcscat(vol, L"\\");
	if(!GetVolumeInformation(
		vol,
		volname,
		MAX_PATH,
		NULL,
		&complen,
		&flags,
		fsysname,
		MAX_PATH))
		return 0;

	return 1;	
}

static wchar_t*
wstrdup(wchar_t *s)
{
	wchar_t *d;
	long n;

	n = (wcslen(s)+1)*sizeof(wchar_t);
	d = mallocz(n, 0);
	memmove(d, s, n);
	return d;
}
	
static Chan*
fsattach(char *spec)
{
	static int devno;
	Ufsinfo *uif;
	Chan *c;

	uif = mallocz(sizeof(Ufsinfo), 1);
	uif->path = wstrdup(L"");

	c = devattach('U', spec);
	c->aux = uif;
	c->dev = devno++;
	c->qid.type = QTDIR;

	return c;
}

static Chan*
fsclone(Chan *c, Chan *nc)
{
	Ufsinfo *uif;

	uif = mallocz(sizeof(Ufsinfo), 1);
	*uif = *(Ufsinfo*)c->aux;
	uif->path = wstrdup(uif->path);
	nc->aux = uif;

	return nc;
}

static int
fswalk1(Chan *c, char *name)
{
	WIN32_FIND_DATA wfd;
	HANDLE h;
	wchar_t *p;
	Ufsinfo *uif;
	
	uif = c->aux;
	p = catpath(uif->path, name, nil);
	switch(pathtype(p)){
	case TPATH_VOLUME:
		if(!checkvolume(p)){
			free(p);
			return 0;
		}
	case TPATH_ROOT:
		c->qid = wfdtoqid(p, nil);
		break;

	case TPATH_FILE:
		if((h = FindFirstFile(p, &wfd)) == INVALID_HANDLE_VALUE){
			free(p);
			return 0;
		}
		FindClose(h);
		c->qid = wfdtoqid(p, &wfd);
		break;
	}
	free(uif->path);
	uif->path = p;
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
	Ufsinfo *uif;

	if(n < BIT16SZ)
		error(Eshortstat);
	uif = c->aux;
	d.name = lastelem(c);
	wfdtodir(uif->path, &d, nil);
	d.type = 'U';
	d.dev = c->dev;
	return convD2M(&d, buf, n);
}

static Chan*
fsopen(Chan *c, int mode)
{
	wchar_t *p;
	Ufsinfo *uif;
	int omode;

	omode = openmode(mode);

	uif = c->aux;
	if(c->qid.type & QTDIR){
		DIR *d;

		if(omode != OREAD)
			error(Eperm);
		d = malloc(sizeof(*d));
		if(waserror()){
			free(d);
			nexterror();
		}
		switch(pathtype(uif->path)){
		case TPATH_ROOT:
			d->drivebuf = malloc(sizeof(wchar_t)*MAX_PATH);
			if(GetLogicalDriveStrings(MAX_PATH-1, d->drivebuf) == 0){
				free(d->drivebuf);
				d->drivebuf = nil;
				oserror();
			}
			d->drivep = d->drivebuf;
			break;
		case TPATH_VOLUME:
		case TPATH_FILE:
			p = catpath(uif->path, "*.*", nil);
			if((d->handle = FindFirstFile(p, &d->wfd)) == INVALID_HANDLE_VALUE){
				free(p);
				oserror();
			}
			free(p);
			break;
		}
		d->keep = 1;
		uif->dir = d;
		poperror();
	} else {
		uif->dir = NULL;
		if((uif->fh = CreateFile(
			uif->path,
			fsaccess(omode & 3),
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			(mode & OTRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			0)) == INVALID_HANDLE_VALUE)
			oserror();
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
	wchar_t *newpath;
	Ufsinfo *uif;
	DIR *d;
	HANDLE h;
	int omode;

	omode = openmode(mode & ~OEXCL);

	uif = c->aux;
	newpath = catpath(uif->path, name, nil);
	if(waserror()){
		free(newpath);
		nexterror();
	}
	d = NULL;
	h = INVALID_HANDLE_VALUE;
	if(perm & DMDIR) {
		wchar_t *p;

		if(omode != OREAD || pathtype(uif->path) == TPATH_ROOT)
			error(Eperm);
		if(!CreateDirectory(newpath, NULL)){
			if(GetLastError() != ERROR_ALREADY_EXISTS || mode & OEXCL)
				oserror();
		}
		d = malloc(sizeof(*d));
		p = catpath(newpath, "*.*", nil);
		if((d->handle = FindFirstFile(p, &d->wfd)) == INVALID_HANDLE_VALUE){
			free(p);
			free(d);
			oserror();
		}
		free(p);
		if(waserror()){
			FindClose(d->handle);
			free(d);
			nexterror();
		}
		d->keep = 1;
	} else {
		if((h = CreateFile(
			newpath,
			fsaccess(omode & 3),
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			(mode & OEXCL) ? CREATE_NEW : CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			0)) == INVALID_HANDLE_VALUE)
			oserror();
		if(waserror()){
			CloseHandle(h);
			nexterror();
		}
	}
	c->qid = wfdtoqid(newpath, nil);
	uif->dir = d;
	uif->fh = h;
	poperror();
	free(uif->path);
	uif->path = newpath;
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
		if(uif->dir != nil){
			if(uif->dir->drivebuf != nil){
				free(uif->dir->drivebuf);
				uif->dir->drivebuf = nil;
			} else {
				FindClose(uif->dir->handle);
				free(uif->dir);
			}
		} else {
			CloseHandle(uif->fh);
		}
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
	HANDLE fh;
	DWORD r;
	Ufsinfo *uif;

	if(c->qid.type & QTDIR)
		return fsdirread(c, va, n, offset);

	uif = c->aux;
	qlock(&uif->oq);
	if(waserror()) {
		qunlock(&uif->oq);
		nexterror();
	}
	fh = uif->fh;
	if(uif->offset != offset) {
		LONG high;
		high = offset>>32;
		offset = SetFilePointer(fh, (LONG)(offset & 0xFFFFFFFF), &high, FILE_BEGIN);
		offset |= (vlong)high<<32;
		uif->offset = offset;
	}
	r = 0;
	if(!ReadFile(fh, va, (DWORD)n, &r, NULL))
		oserror();
	n = r;
	uif->offset += n;
	qunlock(&uif->oq);
	poperror();
	return n;
}

static long
fswrite(Chan *c, void *va, long n, vlong offset)
{
	HANDLE fh;
	DWORD w;
	Ufsinfo *uif;

	if(c->qid.type & QTDIR)
		return fsdirread(c, va, n, offset);

	uif = c->aux;
	qlock(&uif->oq);
	if(waserror()) {
		qunlock(&uif->oq);
		nexterror();
	}
	fh = uif->fh;
	if(uif->offset != offset) {
		LONG high;
		high = offset>>32;
		offset = SetFilePointer(fh, (LONG)(offset & 0xFFFFFFFF), &high, FILE_BEGIN);
		offset |= (vlong)high<<32;
		uif->offset = offset;
	}
	w = 0;
	if(!WriteFile(fh, va, (DWORD)n, &w, NULL))
		oserror();
	n = w;
	uif->offset += n;
	qunlock(&uif->oq);
	poperror();
	return n;
}

static void
fsremove(Chan *c)
{
	Ufsinfo *uif;

	if(waserror()){
		fsclose(c);
		nexterror();
	}
	uif = c->aux;
	if(c->qid.type & QTDIR){
		if(!RemoveDirectory(uif->path))
			oserror();
	} else {
		if(!DeleteFile(uif->path))
			oserror();
	}
	poperror();
	fsclose(c);
}

static int
fswstat(Chan *c, uchar *buf, int n)
{
	char strs[MAX_PATH*3];
	Ufsinfo *uif;
	Dir d;

	if (convM2D(buf, n, &d, strs) != n)
		error(Ebadstat);
	uif = c->aux;
	if(pathtype(uif->path) != TPATH_FILE)
		error(Ebadstat);
	if(~d.atime != 0 || ~d.mtime != 0){
		FILETIME ta, *pta = NULL;
		FILETIME tm, *ptm = NULL;
		HANDLE h;
		if(~d.atime != 0){
			ta = filetime(d.atime);
			pta = &ta;
		}
		if(~d.mtime != 0){
			tm = filetime(d.mtime);
			ptm = &tm;
		}
		if((h = CreateFile(uif->path,
			FILE_WRITE_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			0)) != INVALID_HANDLE_VALUE){
			SetFileTime(h, NULL, pta, ptm);
			CloseHandle(h);
		}
	}
	/* change name */
	if(d.name[0]){
		wchar_t *base, *newpath;
		int l;

		base = wstrdup(uif->path);
		if(waserror()){
			free(base);
			nexterror();
		}
		/* replace last path-element with d.name */
		l = wcslen(base)-1;
		if(l <= 0)
			error(Ebadstat);
		for(;l>0; l--){
			if(base[l-1]=='\\')
				break;
		}
		if(l <= 0)
			error(Ebadstat);
		base[l] = 0;
		newpath = catpath(base, d.name, nil);
		free(base);
		poperror();
		if(waserror()){
			free(newpath);
			nexterror();
		}
		if(wcscmp(uif->path, newpath)!=0){
			if(!MoveFile(uif->path, newpath))
				oserror();
		}
		free(uif->path);
		uif->path = newpath;
		poperror();
	}

	/* fixme: change attributes */
	c->qid = wfdtoqid(uif->path, nil);
	return n;	
}

static wchar_t*
catpath(wchar_t *base, char *cext, wchar_t *wext)
{
	wchar_t *path;
	long n, m;

	n = wcslen(base);
	m = wext!=nil ? wcslen(wext) : strlen(cext)*4;
	path = malloc((n+m+2)*sizeof(wchar_t));
	memmove(path, base, n*sizeof(wchar_t));
	if(n > 0 && path[n-1] != '\\') path[n++] = '\\';
	if(wext != nil)
		memmove(path+n, wext, m*sizeof(wchar_t));
	else
		m = MultiByteToWideChar(CP_UTF8,0,cext,-1,path+n,m);
	path[n+m] = 0;
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

static ulong
fsdirread(Chan *c, uchar *va, int count, vlong offset)
{
	int i;
	ulong t;
	Dir d;
	long n;
	Ufsinfo *uif;
	char de[MAX_PATH*3];
	wchar_t *p;

	i = 0;
	uif = c->aux;
	errno = 0;

	t = pathtype(uif->path);
	if(uif->offset != offset) {
		if(offset != 0)
			error("bad offset in fsdirread");
		uif->offset = offset;  /* sync offset */
		switch(t){
		case TPATH_ROOT:
			uif->dir->drivep = uif->dir->drivebuf;
			break;
		case TPATH_VOLUME:
		case TPATH_FILE:
			FindClose(uif->dir->handle);
			p = catpath(uif->path, "*.*", nil);
			uif->dir->handle = FindFirstFile(p, &uif->dir->wfd);
			free(p);
			if(uif->dir->handle == INVALID_HANDLE_VALUE)
				oserror();
			break;
		}
		uif->dir->keep = 1;
	}

	while(i+BIT16SZ < count) {
		if(!uif->dir->keep) {
			switch(t){
			case TPATH_ROOT:
				uif->dir->drivep += 4;
				if(*uif->dir->drivep == 0)
					goto out;
				break;
			case TPATH_VOLUME:
			case TPATH_FILE:
				if(!FindNextFile(uif->dir->handle, &uif->dir->wfd))
					goto out;
				break;
			}
		} else {
			uif->dir->keep = 0;
		}
		if(t == TPATH_ROOT){
			uif->dir->drivep[2] = 0;
			WideCharToMultiByte(CP_UTF8,0,uif->dir->drivep,-1,de,sizeof(de),0,0);
		} else {
			WideCharToMultiByte(CP_UTF8,0,uif->dir->wfd.cFileName,-1,de,sizeof(de),0,0);
		}
		if(de[0]==0 || isdots(de))
			continue;
		d.name = de;
		if(t == TPATH_ROOT){
			p = catpath(uif->path, nil, uif->dir->drivep);
			wfdtodir(p, &d, nil);
		} else {
			p = catpath(uif->path, nil, uif->dir->wfd.cFileName);
			wfdtodir(p, &d, &uif->dir->wfd);
		}
		free(p);
		d.type = 'U';
		d.dev = c->dev;
		n = convD2M(&d, (uchar*)va+i, count-i);
		if(n == BIT16SZ){
			uif->dir->keep = 1;
			break;
		}
		i += n;
	}
out:
	uif->offset += i;
	return i;
}

static ulong
fsaccess(int m)
{
	ulong a;
	a = 0;
	switch(m & 3){
	default:
		error(Eperm);
		break;
	case OREAD:
		a = GENERIC_READ;
		break;
	case OWRITE:
		a = GENERIC_WRITE;
		break;
	case ORDWR:
		a = GENERIC_READ | GENERIC_WRITE;
		break;
	}
	return a;
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
