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

/*
 *  if waking a sleeping process, this routine must hold both
 *  p->rlock and r->lock.  However, it can't know them in
 *  the same order as wakeup causing a possible lock ordering
 *  deadlock.  We break the deadlock by giving up the p->rlock
 *  lock if we can't get the r->lock and retrying.
 */
void
procinterrupt(Proc *p)
{
	int s;

	p->notepending = 1;

	/* this loop is to avoid lock ordering problems. */
	for(;;){
		Rendez *r;

		s = splhi();
		lock(&p->rlock);
		r = p->r;

		/* waiting for a wakeup? */
		if(r == nil)
			break;	/* no */

		/* try for the second lock */
		if(canlock(&r->lk)){
			if(p->state != Wakeme || r->p != p)
				panic("procinterrupt: state %d %d %d",
					r->p != p, p->r != r, p->state);
			p->r = nil;
			r->p = nil;
			unlock(&p->rlock);
			unlock(&r->lk);
			/* hands off r */
			procwakeup(p);
			splx(s);
			return;
		}

		/* give other process time to get out of critical section and try again */
		unlock(&p->rlock);
		splx(s);

		osyield();
	}
	unlock(&p->rlock);
	splx(s);

	switch(p->state){
	case Rendezvous:
		/* Try and pull out of a rendezvous */
		lock(&p->rgrp->ref.lk);
		if(p->state == Rendezvous) {
			Proc *d, **l;

			l = &REND(p->rgrp, (uintptr)p->rendtag);
			for(d = *l; d != nil; d = d->rendhash) {
				if(d == p) {
					*l = p->rendhash;
					p->rendval = (void*)~(uintptr)0;
					unlock(&p->rgrp->ref.lk);
					/* hands off p->rgrp */
					procwakeup(p);
					return;
				}
				l = &d->rendhash;
			}
		}
		unlock(&p->rgrp->ref.lk);
		break;
	}
}

int
postnote(Proc *p, int x, char *msg, int flag)
{
	USED(x);
	USED(msg);
	USED(flag);
	procinterrupt(p);
	return 0;
}
