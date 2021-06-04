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
	pexit("", 0);
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
osexit(void)
{
	ExitThread(0);
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

/*
 * Quote a single command line argument using the rules above.
 */
static char*
qarg(char *s)
{
	char *d, *p;
	int n, c;

	n = strlen(s);
	d = smalloc(3+2*n);
	for(p = s; (c = *p) != 0; p++)
		if(strchr(" \t\n\r\"", c) != nil)
			break;
	if(c == 0 && p != s){
		memmove(d, s, n+1);
		return d;
	}
	p = d;
	*p++ = '"';
	for(;;){
		for(n = 0; (c = *s++) == '\\'; n++)
			*p++ = c;
		if(c == 0){
			while(n-- > 0)
				*p++ = '\\';
			break;
		}
		if(c == '"'){
			while(n-- >= 0)
				*p++ = '\\';
		}
		*p++ = c;
	}
	*p++ = '"';
	*p = 0;
	return d;
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
	arg = smalloc(n);
	WideCharToMultiByte(CP_UTF8, 0, warg, -1, arg, n, 0, 0);

	/* conservative guess at the number of args */
	for(argc=4,p=arg; *p; p++)
		if(*p == ' ' || *p == '\t')
			argc++;
	argv = smalloc(argc*sizeof(char*));
	argc = args(argv, argc, arg);

	main(argc, argv);
	ExitThread(0);
	return 0;
}


static wchar_t*
wcmdline(char **argv)
{
	wchar_t *s, *w, *e;
	int n, i;
	char *q;

	n = 0;
	for(i = 0; argv[i] != nil; i++){
		q = qarg(argv[i]);
		n += strlen(q)+1;
		free(q);
	}
	s = smalloc((n+1)*sizeof(wchar_t));
	w = s;
	e = s + n;
	for(i = 0; argv[i] != nil; i++){
		if(i != 0)
			*w++ = L' ';
		q = qarg(argv[i]);
		w += MultiByteToWideChar(CP_UTF8, 0, q, strlen(q), w, e - w);
		free(q);
	}
	*w = 0;
	return s;
}

void*
oscmd(char **argv, int nice, char *dir, Chan **fd)
{
	SECURITY_ATTRIBUTES sa;
	PROCESS_INFORMATION pi;
	STARTUPINFOW si;
	HANDLE p[3][2], tmp;
	wchar_t *wcmd, *wdir;
	int i;

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	for(i = 0; i < 3; i++){
		if(!CreatePipe(&p[i][i==0], &p[i][i!=0], &sa, 0)
		|| !DuplicateHandle(GetCurrentProcess(), p[i][0], GetCurrentProcess(), &tmp, 0, FALSE, DUPLICATE_SAME_ACCESS)){
			while(--i >= 0){
				CloseHandle(p[i][0]);
				CloseHandle(p[i][1]);
			}
			oserror();
		}
		CloseHandle(p[i][0]);
		p[i][0] = tmp;
	}

	if(waserror()){
		for(i = 0; i < 3; i++){
			CloseHandle(p[i][0]);
			CloseHandle(p[i][1]);
		}
		nexterror();
	}

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = p[0][1];
	si.hStdOutput = p[1][1];
	si.hStdError = p[2][1];
	si.lpDesktop = L"";

	i = strlen(dir)+1;
	wdir = smalloc(i*sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, dir, i, wdir, i);

	wcmd = wcmdline(argv);
	if(waserror()){
		free(wcmd);
		nexterror();
	}

	if(!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW, NULL, wdir, &si, &pi))
		oserror();

	poperror();
	free(wcmd);
	free(wdir);

	poperror();
	for(i = 0; i < 3; i++){
		fd[i] = lfdchan((void*)p[i][0]);
		CloseHandle(p[i][1]);
	}
	CloseHandle(pi.hThread);
	return (void*)pi.hProcess;
}

int
oscmdwait(void *c, char *status, int nstatus)
{
	DWORD code = -1;
	for(;;){
		if(!GetExitCodeProcess((HANDLE)c, &code))
			return -1;
		if(code != STILL_ACTIVE)
			break;
		WaitForSingleObject((HANDLE)c, INFINITE);
	}
	if(code == 0)
		return snprint(status, nstatus, "0 0 0 0 ''");
	return snprint(status, nstatus, "0 0 0 0 %d", (int)code);
}

int
oscmdkill(void *c)
{
	TerminateProcess((HANDLE)c, 0);
	return 0;
}

void
oscmdfree(void *c)
{
	CloseHandle((HANDLE)c);
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

	cmd = smalloc((n+1)*sizeof(wchar_t));
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

void
setterm(int raw)
{
	DWORD mode;
	HANDLE h;

	h = GetStdHandle(STD_INPUT_HANDLE);
	if(!GetConsoleMode(h, &mode))
		return;
	if(raw)
		mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
	else
		mode |= (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
	SetConsoleMode(h, mode);
	FlushConsoleInputBuffer(h);
	_setmode(0, raw? _O_BINARY: _O_TEXT);
}
