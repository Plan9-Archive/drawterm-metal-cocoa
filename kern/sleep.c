#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	int s;

	s = splhi();

	lock(&r->lk);
	lock(&up->rlock);
	if(r->p){
		print("double sleep %lud %lud\n", r->p->pid, up->pid);
	}

	/*
	 *  Wakeup only knows there may be something to do by testing
	 *  r->p in order to get something to lock on.
	 *  Flush that information out to memory in case the sleep is
	 *  committed.
	 */
	r->p = up;

	if((*f)(arg) || up->notepending){
		/*
		 *  if condition happened or a note is pending
		 *  never mind
		 */
		r->p = nil;
		unlock(&up->rlock);
		unlock(&r->lk);
	} else {
		/*
		 *  now we are committed to
		 *  change state and call scheduler
		 */
		up->state = Wakeme;
		up->r = r;

		/* statistics */
		/* m->cs++; */

		unlock(&up->rlock);
		unlock(&r->lk);

		procsleep();
	}

	if(up->notepending) {
		up->notepending = 0;
		splx(s);
		error(Eintr);
	}

	splx(s);
}

Proc*
wakeup(Rendez *r)
{
	Proc *p;
	int s;

	s = splhi();

	lock(&r->lk);
	p = r->p;

	if(p == nil)
		unlock(&r->lk);
	else {
		lock(&p->rlock);
		if(p->state != Wakeme || p->r != r)
			panic("wakeup: state");
		p->state = Running;
		r->p = nil;
		p->r = nil;
		unlock(&p->rlock);
		unlock(&r->lk);
		procwakeup(p);
	}
	splx(s);

	return p;
}

