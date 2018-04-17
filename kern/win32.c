#include <windows.h>
#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"

typedef struct Oproc Oproc;
struct Oproc {
	int tid;
	HANDLE	*sema;
};

static int tlsx = TLS_OUT_OF_INDEXES;

char	*argv0;

Proc*
_getproc(void)
{
	if(tlsx == TLS_OUT_OF_INDEXES)
		return nil;
	return TlsGetValue(tlsx);
}

void
_setproc(Proc *p)
{
	if(tlsx == TLS_OUT_OF_INDEXES){
		tlsx = TlsAlloc();
		if(tlsx == TLS_OUT_OF_INDEXES)
			panic("out of indexes");
	}
	TlsSetValue(tlsx, p);
}

void
oserror(void)
{
	oserrstr();
	nexterror();
}

void
osinit(void)
{
	Oproc *t;
	static Proc firstprocCTstore;

	_setproc(&firstprocCTstore);
	t = (Oproc*)firstprocCTstore.oproc;
	assert(t != 0);

	t->tid = GetCurrentThreadId();
	t->sema = CreateSemaphore(0, 0, 1000, 0);
	if(t->sema == 0) {
		oserror();
		panic("could not create semaphore: %r");
	}
}

void
osnewproc(Proc *p)
{
	Oproc *op;

	op = (Oproc*)p->oproc;
	op->sema = CreateSemaphore(0, 0, 1000, 0);
	if (op->sema == 0) {
		oserror();
		panic("could not create semaphore: %r");
	}
}

void
osmsleep(int ms)
{
	Sleep((DWORD) ms);
}

void
osyield(void)
{
	Sleep(0);
}

static DWORD WINAPI
tramp(LPVOID vp)
{
	Proc *p = (Proc *) vp;
	Oproc *op = (Oproc*) p->oproc;

	_setproc(p);
	op->tid = GetCurrentThreadId();
 	(*p->fn)(p->arg);
	ExitThread(0);
	return 0;
}

void
osproc(Proc *p)
{
	DWORD tid;

	if(CreateThread(0, 0, tramp, p, 0, &tid) == 0) {
		oserror();
		panic("osproc: %r");
	}
}

void
procsleep(void)
{
	Proc *p;
	Oproc *op;

	p = up;
	op = (Oproc*)p->oproc;
	WaitForSingleObject(op->sema, INFINITE);}

void
procwakeup(Proc *p)
{
	Oproc *op;

	op = (Oproc*)p->oproc;
	ReleaseSemaphore(op->sema, 1, 0);
}

BOOLEAN WINAPI (*RtlGenRandom)(PVOID, ULONG);

void
randominit(void)
{
	HMODULE mod;
	
	mod = LoadLibraryW(L"ADVAPI32.DLL");
	if(mod != NULL)
		RtlGenRandom = (void *) GetProcAddress(mod, "SystemFunction036");
}

ulong
randomread(void *v, ulong n)
{
	RtlGenRandom(v, n);
	return n;
}

#undef time
long
seconds(void)
{
	return time(0);
}

ulong
ticks(void)
{
	return GetTickCount();
}

int
wstrutflen(Rune *s)
{
	int n;
	
	for(n=0; *s; n+=runelen(*s),s++)
		;
	return n;
}

int
wstrtoutf(char *s, Rune *t, int n)
{
	int i;
	char *s0;

	s0 = s;
	if(n <= 0)
		return wstrutflen(t)+1;
	while(*t) {
		if(n < UTFmax+1 && n < runelen(*t)+1) {
			*s = 0;
			return s-s0+wstrutflen(t)+1;
		}
		i = runetochar(s, t);
		s += i;
		n -= i;
		t++;
	}
	*s = 0;
	return s-s0;
}

/*
 * Break the command line into arguments
 * The rules for this are not documented but appear to be the following
 * according to the source for the microsoft C library.
 * Words are seperated by space or tab
 * Words containing a space or tab can be quoted using "
 * 2N backslashes + " ==> N backslashes and end quote
 * 2N+1 backslashes + " ==> N backslashes + literal "
 * N backslashes not followed by " ==> N backslashes
 */
static int
args(char *argv[], int n, char *p)
{
	char *p2;
	int i, j, quote, nbs;

	for(i=0; *p && i<n-1; i++) {
		while(*p == ' ' || *p == '\t')
			p++;
		quote = 0;
		argv[i] = p2 = p;
		for(;*p; p++) {
			if(!quote && (*p == ' ' || *p == '\t'))
				break;
			for(nbs=0; *p == '\\'; p++,nbs++)
				;
			if(*p == '"') {
				for(j=0; j<(nbs>>1); j++)
					*p2++ = '\\';
				if(nbs&1)
					*p2++ = *p;
				else
					quote = !quote;
			} else {
				for(j=0; j<nbs; j++)
					*p2++ = '\\';
				*p2++ = *p;
			}
		}
		/* move p up one to avoid pointing to null at end of p2 */
		if(*p)
			p++;
		*p2 = 0;	
	}
	argv[i] = 0;

	return i;
}

extern int	main(int, char*[]);

int APIENTRY
WinMain(HINSTANCE x, HINSTANCE y, LPSTR z, int w)
{
	int argc, n;
	char *arg, *p, **argv;
	wchar_t *warg;

	warg = GetCommandLineW();
	n = wcslen(warg)*UTFmax+1;
	arg = malloc(n);
	WideCharToMultiByte(CP_UTF8,0,warg,-1,arg,n,0,0);

	/* conservative guess at the number of args */
	for(argc=4,p=arg; *p; p++)
		if(*p == ' ' || *p == '\t')
			argc++;
	argv = malloc(argc*sizeof(char*));
	argc = args(argv, argc, arg);

	main(argc, argv);
	ExitThread(0);
	return 0;
}

void
oserrstr(void)
{
	char *p, *q;
	int e, r;

	e = GetLastError();
	p = up->errstr;	/* up kills last error */
	r = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM,
		0, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		p, ERRMAX, 0);
	if(r == 0)
		snprint(p, ERRMAX, "windows error %d", e);
	for(q=p; *p; p++) {
		if(*p == '\r')
			continue;
		if(*p == '\n')
			*q++ = ' ';
		else
			*q++ = *p;
	}
	*q = '\0';
}

long
showfilewrite(char *a, int n)
{
	wchar_t *action, *arg, *cmd, *p;
	int m;

	cmd = malloc((n+1)*sizeof(wchar_t));
	if(cmd == nil)
		error("out of memory");
	m = MultiByteToWideChar(CP_UTF8,0,a,n,cmd,n);
	while(m > 0 && cmd[m-1] == '\n')
		m--;
	cmd[m] = 0;
	p = wcschr(cmd, ' ');
	if(p){
		action = cmd;
		*p++ = 0;
		arg = p;
	}else{
		action = L"open";
		arg = cmd;
	}
	ShellExecuteW(0, action, arg, 0, 0, SW_SHOWNORMAL);
	free(cmd);
	return n;
}
