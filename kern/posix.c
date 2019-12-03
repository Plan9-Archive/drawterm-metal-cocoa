/*
 * Posix generic OS implementation for drawterm.
 */

#include "u.h"

#ifndef _XOPEN_SOURCE	/* for Apple and OpenBSD; not sure if needed */
#define _XOPEN_SOURCE 500
#endif

#include <pthread.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <termios.h>

#include "lib.h"
#include "dat.h"
#include "fns.h"

typedef struct Oproc Oproc;
struct Oproc
{
	int nsleep;
	int nwakeup;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

static pthread_key_t prdakey;

Proc*
_getproc(void)
{
	void *v;

	if((v = pthread_getspecific(prdakey)) == nil)
		panic("cannot getspecific");
	return v;
}

void
_setproc(Proc *p)
{
	if(pthread_setspecific(prdakey, p) != 0)
		panic("cannot setspecific");
}

void
osinit(void)
{
	if(pthread_key_create(&prdakey, 0))
		panic("cannot pthread_key_create");

	signal(SIGPIPE, SIG_IGN);
}

void
osnewproc(Proc *p)
{
	Oproc *op;
	pthread_mutexattr_t attr;

	op = (Oproc*)p->oproc;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
	pthread_mutex_init(&op->mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	pthread_cond_init(&op->cond, 0);
}

void
osmsleep(int ms)
{
	struct timeval tv;

	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000; /* micro */
	if(select(0, NULL, NULL, NULL, &tv) < 0)
		panic("select");
}

void
osyield(void)
{
	sched_yield();
}

void
oserrstr(void)
{
	char *p;

	if((p = strerror(errno)) != nil)
		strecpy(up->errstr, up->errstr+ERRMAX, p);
	else
		snprint(up->errstr, ERRMAX, "unix error %d", errno);
}

void
oserror(void)
{
	oserrstr();
	nexterror();
}

static void*
tramp(void *vp)
{
	Proc *p;

	p = vp;
	if(pthread_setspecific(prdakey, p))
		panic("cannot setspecific");
	(*p->fn)(p->arg);
	pexit("", 0);
	return 0;
}

void
osproc(Proc *p)
{
	pthread_t pid;

	if(pthread_create(&pid, nil, tramp, p)){
		oserrstr();
		panic("osproc: %r");
	}
	sched_yield();
}

void
osexit(void)
{
	pthread_setspecific(prdakey, 0);
	pthread_exit(0);
}

void
procsleep(void)
{
	Proc *p;
	Oproc *op;

	p = up;
	op = (Oproc*)p->oproc;
	pthread_mutex_lock(&op->mutex);
	op->nsleep++;
	while(op->nsleep > op->nwakeup)
		pthread_cond_wait(&op->cond, &op->mutex);
	pthread_mutex_unlock(&op->mutex);
}

void
procwakeup(Proc *p)
{
	Oproc *op;

	op = (Oproc*)p->oproc;
	pthread_mutex_lock(&op->mutex);
	op->nwakeup++;
	if(op->nwakeup == op->nsleep)
		pthread_cond_signal(&op->cond);
	pthread_mutex_unlock(&op->mutex);
}

#undef chdir
#undef pipe
#undef fork
#undef close
void*
oscmd(char **argv, int nice, char *dir, Chan **fd)
{
	int p[3][2];
	int i, pid;

	for(i = 0; i<3; i++){
		if(pipe(p[i]) < 0){
			while(--i >= 0){
				close(p[i][0]);
				close(p[i][1]);
			}
			oserror();
		}
	}
	if(waserror()){
		for(i = 0; i < 3; i++){
			close(p[i][0]);
			close(p[i][1]);
		}
		nexterror();
	}
	pid = fork();
	if(pid == -1)
		oserror();
	if(pid == 0){
		setsid();
		dup2(p[0][0], 0);
		dup2(p[1][1], 1);
		dup2(p[2][1], 2);
		for(i = 3; i < 1000; i++)
			close(i);
		if(chdir(dir) < 0){
			perror("chdir");
			_exit(1);
		}
		execvp(argv[0], argv);
		perror("exec");
		_exit(1);
	}
	poperror();
	close(p[0][0]);
	close(p[1][1]);
	close(p[2][1]);

	fd[0] = lfdchan((void*)(uintptr)p[0][1]);
	fd[1] = lfdchan((void*)(uintptr)p[1][0]);
	fd[2] = lfdchan((void*)(uintptr)p[2][0]);

	return (void*)(uintptr)pid;
}

int
oscmdwait(void *c, char *status, int nstatus)
{
	int pid = (int)(uintptr)c;
	int s = -1;

	if(waitpid(pid, &s, 0) < 0)
		return -1;
	if(WIFEXITED(s)){
		if((s = WEXITSTATUS(s)) == 0)
			return snprint(status, nstatus, "%d 0 0 0 ''", pid);
		return snprint(status, nstatus, "%d 0 0 0 'exit: %d'", pid, s);
	}
	if(WIFSIGNALED(s)){
		switch(s = WTERMSIG(s)){
		case SIGTERM:
		case SIGKILL:
			return snprint(status, nstatus, "%d 0 0 0 killed", pid);
		}
		return snprint(status, nstatus, "%d 0 0 0 'signal: %d'", pid, s);
	}
	return snprint(status, nstatus, "%d 0 0 0 'odd status: 0x%x'", pid, s);
}

int
oscmdkill(void *c)
{
	int pid = (int)(uintptr)c;
	return kill(pid, SIGTERM);
}

void
oscmdfree(void *c)
{
	USED(c);
}

static int randfd;
#undef open
void
randominit(void)
{
	if((randfd = open("/dev/urandom", OREAD)) < 0)
	if((randfd = open("/dev/random", OREAD)) < 0)
		panic("open /dev/random: %r");
}

#undef read
ulong
randomread(void *v, ulong n)
{
	int m;

	if((m = read(randfd, v, n)) != n)
		panic("short read from /dev/random: %d but %d", n, m);
	return m;
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
	static long sec0 = 0, usec0;
	struct timeval t;

	if(gettimeofday(&t, nil) < 0)
		return 0;
	if(sec0 == 0){
		sec0 = t.tv_sec;
		usec0 = t.tv_usec;
	}
	return (t.tv_sec-sec0)*1000+(t.tv_usec-usec0+500)/1000;
}

long
showfilewrite(char *a, int n)
{
	error("not implemented");
	return -1;
}

void
setterm(int raw)
{
	struct termios t;

	if(tcgetattr(0, &t) < 0)
		return;
	if(raw)
		t.c_lflag &= ~(ECHO|ICANON);
	else
		t.c_lflag |= (ECHO|ICANON);
	tcsetattr(0, TCSAFLUSH, &t);
}
