#include	<windows.h>
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<fcntl.h>

#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

typedef struct DIR	DIR;
typedef	struct Ufsinfo	Ufsinfo;


enum
{
	MAXPATH	= 1024,
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
	TCHAR		*drivebuf;
	TCHAR		*drivep;

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
};

static	void	fspath(Chan *, char *, TCHAR *, int);
static	ulong	fsdirread(Chan*, uchar*, int, vlong);
static	int	fsomode(int);
static	ulong	fsaccess(int);
static	ulong	pathtype(TCHAR *);
static	int	checkvolume(TCHAR *);

char *base = "";

static ulong
unixtime(FILETIME *ft)
{
	vlong t;
	t = ((vlong)ft->dwHighDateTime << 32)|((vlong)ft->dwLowDateTime);
	t -= 116444736000000000LL;
	return ((t<0)?(-1 - (-t - 1)) : t)/10000000;
}

static uvlong
pathhash(TCHAR *p)
{
	uvlong h;
	h = 0LL;
	for(; *p; p++)
		h += *p * 13;
	return h;
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
wfdtoqid(TCHAR *path, WIN32_FIND_DATA *wfd)
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
wfdtodir(TCHAR *path, Dir *d, WIN32_FIND_DATA *wfd)
{
	WIN32_FIND_DATA f;

	d->uid	= "nul";
	d->gid	= "nul";
	d->muid	= "nul";

	switch(pathtype(path)){
	case TPATH_VOLUME:
	case TPATH_ROOT:
		wfd = nil;
		d->mode = 0777 | DMDIR;
		d->atime = 0;
		d->mtime = 0;
		d->length = 0;
		break;

	case TPATH_FILE:
		if(!wfd){
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
}

/* clumsy hack, but not worse than the Path stuff in the last one */
static char*
uc2name(Chan *c)
{
	char *s;

	if(c->name == nil)
		return "";
	s = c2name(c);
	if(s[0]=='#' && s[1]=='U')
		s += 2;
	if(*s=='/')
		s++;
	return s;
}

static char*
lastelem(Chan *c)
{
	char *s, *t;

	s = uc2name(c);
	if((t = strrchr(s, '/')) == nil)
		return s;
	if(t[1] == 0)
		return t;
	return t+1;
}

static ulong
pathtype(TCHAR *path)
{
	int n;
	n = wcslen(path);
	if(n < 2){
		return TPATH_ROOT;
	}
	if(n==2){
		return TPATH_VOLUME;
	}
	return TPATH_FILE;
}

static int
checkvolume(TCHAR *path)
{
	TCHAR vol[MAX_PATH];
	TCHAR volname[MAX_PATH];
	TCHAR fsysname[MAX_PATH];
	DWORD complen;
	DWORD flags;

	wcsncpy(vol, path, MAX_PATH);
#ifdef UNICODE
	wcsncat(vol, L"\\", MAXPATH);
#else
	wcsncat(vol, "\\", MAXPATH);
#endif

	if(!GetVolumeInformation(
		vol,
		volname,
		MAX_PATH,
		NULL,
		&complen,
		&flags,
		fsysname,
		MAX_PATH)){
		return 0;
	}
	return 1;	
}

static int
getfileowner(TCHAR *path, char *owner, int nowner)
{
	strncpy(owner, "Bill", nowner);
	return 1;
}
	
static Chan*
fsattach(char *spec)
{
	Chan *c;
	static int devno;
	Ufsinfo *uif;
	c = devattach('U', spec);
	uif = mallocz(sizeof(Ufsinfo), 1);
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
	nc->aux = uif;

	return nc;
}

static int
fswalk1(Chan *c, char *name)
{
	HANDLE h;
	WIN32_FIND_DATA wfd;
	TCHAR path[MAXPATH];

	fspath(c, name, path, MAXPATH);

	switch(pathtype(path)){
	case TPATH_VOLUME:
		if(!checkvolume(path))
			return 0;
	case TPATH_ROOT:
		c->qid = wfdtoqid(path, nil);
		break;

	case TPATH_FILE:
		if((h = FindFirstFile(path, &wfd)) == INVALID_HANDLE_VALUE)
			return 0;
		FindClose(h);
		c->qid = wfdtoqid(path, &wfd);
		break;
	}
	return 1;
}

extern Cname* addelem(Cname*, char*);

static Walkqid*
fswalk(Chan *c, Chan *nc, char **name, int nname)
{
	int i;
	Cname *cname;
	Walkqid *wq;

	if(nc != nil)
		panic("fswalk: nc != nil");
	wq = smalloc(sizeof(Walkqid)+(nname-1)*sizeof(Qid));
	nc = devclone(c);
	cname = c->name;
	incref(&cname->ref);

	fsclone(c, nc);
	wq->clone = nc;
	for(i=0; i<nname; i++){
		nc->name = cname;
		if(fswalk1(nc, name[i]) == 0)
			break;
		cname = addelem(cname, name[i]);
		wq->qid[i] = nc->qid;
	}
	nc->name = cname;
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
	TCHAR path[MAXPATH];
	char owner[MAXPATH];

	if(n < BIT16SZ)
		error(Eshortstat);
	fspath(c, 0, path, MAXPATH);
	d.name = lastelem(c);
	wfdtodir(path, &d, nil);

	if(getfileowner(path, owner, MAXPATH)){
		d.uid = owner;
		d.gid = owner;
		d.muid = owner;
	}

	d.type = 'U';
	d.dev = c->dev;
	return convD2M(&d, buf, n);
}

static Chan*
fsopen(Chan *c, int mode)
{
	TCHAR path[MAXPATH];
	ulong t;
	int m, isdir;
	Ufsinfo *uif;

	m = mode & (OTRUNC|3);
	switch(m) {
	case 0:
		break;
	case 1:
	case 1|16:
		break;
	case 2:	
	case 0|16:
	case 2|16:
		break;
	case 3:
		break;
	default:
		error(Ebadarg);
	}

	isdir = c->qid.type & QTDIR;
	if(isdir && mode != OREAD)
		error(Eperm);
	m = fsomode(m & 3);
	c->mode = openmode(mode);
	uif = c->aux;
	uif->offset = 0;
	fspath(c, 0, path, MAXPATH);
	t = pathtype(path);
	if(isdir){
		DIR *d;
		d = malloc(sizeof(*d));
		switch(t){
		case TPATH_ROOT:
			d->drivebuf = malloc(sizeof(TCHAR)*MAX_PATH);
			if(GetLogicalDriveStrings(MAX_PATH-1, d->drivebuf) == 0){
				free(d->drivebuf);
				d->drivebuf = nil;
				oserror();
			}
			d->drivep = d->drivebuf;
			break;
		case TPATH_VOLUME:
		case TPATH_FILE:
#ifdef UNICODE
			wcsncat(path, L"\\*.*", MAXPATH);
#else
			wcsncat(path, "\\*.*", MAXPATH);
#endif
			if((d->handle = FindFirstFile(path, &d->wfd)) == INVALID_HANDLE_VALUE){
				free(d);
				oserror();
			}
			break;
		}
		d->keep = 1;
		uif->dir = d;
	} else {
		uif->dir = nil;
		if((uif->fh = CreateFile(
			path,
			fsaccess(mode),
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			(mode & OTRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			0)) == INVALID_HANDLE_VALUE){
			oserror();
		}	
	}
	c->offset = 0;
	c->flag |= COPEN;
	return c;
}

static void
fscreate(Chan *c, char *name, int mode, ulong perm)
{
	int m;
	TCHAR path[MAXPATH];
	Ufsinfo *uif;
	ulong t;

	m = fsomode(mode&3);

	fspath(c, name, path, MAXPATH);
	t = pathtype(path);

	uif = c->aux;

	if(perm & DMDIR) {
		TCHAR *p;
		DIR *d;
		if(m || t!=TPATH_FILE)
			error(Eperm);
		if(!CreateDirectory(path, NULL))
			oserror();
		d = malloc(sizeof(*d));
		p = &path[wcslen(path)];
#ifdef UNICODE
		wcsncat(path, L"\\*.*", MAXPATH);
#else
		wcsncat(path, "\\*.*", MAXPATH);
#endif
		if((d->handle = FindFirstFile(path, &d->wfd)) == INVALID_HANDLE_VALUE){
			free(d);
			oserror();
		}
		*p = 0;
		d->keep = 1;
		uif->dir = d;
	} else {
		uif->dir = nil;
		if((uif->fh = CreateFile(
			path,
			fsaccess(mode),
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL,
			0)) == INVALID_HANDLE_VALUE){
			oserror();
		}	
	}
	c->qid = wfdtoqid(path, nil);
	c->offset = 0;
	c->flag |= COPEN;
	c->mode = openmode(mode);
}


static void
fsclose(Chan *c)
{
	Ufsinfo *uif;

	uif = c->aux;
	if(c->flag & COPEN) {
		if(uif->dir){
			if(uif->dir->drivebuf){
				free(uif->dir->drivebuf);
				uif->dir->drivebuf = nil;
			} else {
				FindClose(uif->dir->handle);
				free(uif->dir);
			}
		} else {
			CloseHandle(uif->fh);
		}
	}
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
	if(!ReadFile(fh, va, (DWORD)n, &r, NULL)){
		oserror();
	}
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
	if(!WriteFile(fh, va, (DWORD)n, &w, NULL)){
		oserror();
	}
	n = w;
	uif->offset += n;
	qunlock(&uif->oq);
	poperror();
	return n;
}

static void
fsremove(Chan *c)
{
	TCHAR path[MAXPATH];

	fspath(c, 0, path, MAXPATH);
	if(c->qid.type & QTDIR){
		if(!RemoveDirectory(path))
			error("RemoveDirectory...");
	} else {
		if(!DeleteFile(path))
			error("DeleteFile...");
	}
}

static int
fswstat(Chan *c, uchar *buf, int n)
{
	TCHAR path[MAXPATH];
	char strs[MAXPATH*3];
	Dir d;
	ulong t;

	if (convM2D(buf, n, &d, strs) != n)
		error(Ebadstat);
	fspath(c, 0, path, MAXPATH);
	t = pathtype(path);
	if(t != TPATH_FILE)
		error(Ebadstat);
	/* change name */
	if(d.name[0]){
		int l;
		TCHAR newpath[MAXPATH];
		wcsncpy(newpath, path, MAXPATH);

		/* replace last path-element with d.name */
		l = wcslen(newpath)-1;
		if(l <= 0)
			error(Ebadstat);
		for(;l>0; l--){
			if(newpath[l-1]=='\\')
				break;
		}
		if(l <= 0)
			error(Ebadstat);
		newpath[l] = 0;
#ifdef UNICODE
		if(!MultiByteToWideChar(CP_UTF8,0,d.name,-1,&newpath[l],MAXPATH-l-1))
			oserror();
#else
		wcsncpy(&newpath[l], d.name, MAXPATH-l-1);
#endif
		if(wcscmp(path, newpath)!=0){
			if(!MoveFile(path, newpath))
				oserror();
			wcsncpy(path, newpath, MAXPATH);
		}
	}

	/* fixme: change attributes */
	c->qid = wfdtoqid(path, nil);
	return n;	
}


static void
_fspath(Chan *c, char *ext, char *path, int npath)
{
	*path = 0;
	strncat(path, uc2name(c), npath);
	if(ext) {
		if(*path)
			strncat(path, "/", npath);
		strncat(path, ext, npath);
	}
	cleanname(path);
	if(*path == '.')
		*path = 0;
}

static void
fspath(Chan *c, char *ext, TCHAR *path, int npath)
{
	TCHAR *p;
#ifdef UNICODE
	char buf[MAXPATH];
	_fspath(c, ext, buf, sizeof(buf));
	if(!MultiByteToWideChar(CP_UTF8,0,buf,-1,path,npath)){
		oserror();
	}
#else
	_fspath(c, ext, path, npath);
#endif
	/* make a DOS path */
	for(p=path; *p; p++){
		if(*p == '/')
			*p = '\\';
	}
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
	int ndirpath;
	TCHAR dirpath[MAXPATH];

	i = 0;
	uif = c->aux;
	errno = 0;

	fspath(c, 0, dirpath, MAXPATH);
	ndirpath = wcslen(dirpath);
	t = pathtype(dirpath);

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
#ifdef UNICODE
			wcsncat(dirpath, L"\\*.*", MAXPATH);
#else
			wcsncat(dirpath, "\\*.*", MAXPATH);
#endif
			if((uif->dir->handle = FindFirstFile(dirpath, &uif->dir->wfd))==INVALID_HANDLE_VALUE){
				oserror();
			}
			break;
		}
		uif->dir->keep = 1;
	}

	while(i+BIT16SZ < count) {
		char owner[MAXPATH];

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
#ifdef UNICODE
			WideCharToMultiByte(CP_UTF8,0,uif->dir->drivep,-1,de,sizeof(de),0,0);
#else
			strncpy(de, uif->dir->drivep, sizeof(de));
#endif
		} else {
#ifdef UNICODE
			WideCharToMultiByte(CP_UTF8,0,uif->dir->wfd.cFileName,-1,de,sizeof(de),0,0);
#else
			strncpy(de, uif->dir->wfd.cFileName, sizeof(de));
#endif
		}
		if(de[0]==0 || isdots(de))
			continue;
		d.name = de;
		dirpath[ndirpath] = 0;		
		if(t == TPATH_ROOT){
			wcsncat(dirpath, uif->dir->drivep, MAXPATH);
			wfdtodir(dirpath, &d, nil);
		} else {
#ifdef UNICODE
			wcsncat(dirpath, L"\\", MAXPATH);
#else
			wcsncat(dirpath, "\\", MAXPATH);
#endif
			wcsncat(dirpath, uif->dir->wfd.cFileName, MAXPATH);
			wfdtodir(dirpath, &d, &uif->dir->wfd);
		}

		if(getfileowner(dirpath, owner, MAXPATH)){
			d.uid = owner;
			d.gid = owner;
			d.muid = owner;
		}

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

static int
fsomode(int m)
{
	switch(m) {
	case 0:			/* OREAD */
	case 3:			/* OEXEC */
		return 0;
	case 1:			/* OWRITE */
		return 1;
	case 2:			/* ORDWR */
		return 2;
	}
	error(Ebadarg);
	return 0;
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
