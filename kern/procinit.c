#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

void
procinit0(void)
{
	Proc *p;

	p = newproc();
	p->fgrp = dupfgrp(nil);
	p->rgrp = newrgrp();
	p->pgrp = newpgrp();
	_setproc(p);

	up->slash = namec("#/", Atodir, 0, 0);
	pathclose(up->slash->path);
	up->slash->path = newpath("/");
	up->dot = cclone(up->slash);
}

Ref pidref;

Proc*
newproc(void)
{
	Proc *p;

	p = mallocz(sizeof(Proc), 1);
	p->pid = incref(&pidref);
	strcpy(p->user, eve);
	p->syserrstr = p->errbuf0;
	p->errstr = p->errbuf1;
	strcpy(p->text, "drawterm");
	osnewproc(p);
	return p;
}

void*
kproc(char *name, void (*fn)(void*), void *arg)
{
	Proc *p;

	p = newproc();
	p->fn = fn;
	p->arg = arg;
	p->slash = cclone(up->slash);
	p->dot = cclone(up->dot);
	p->rgrp = up->rgrp;
	if(p->rgrp != nil)
		incref(&p->rgrp->ref);
	p->pgrp = up->pgrp;
	if(up->pgrp != nil)
		incref(&up->pgrp->ref);
	p->fgrp = up->fgrp;
	if(p->fgrp != nil)
		incref(&p->fgrp->ref);
	strecpy(p->text, p->text+sizeof p->text, name);

	osproc(p);
	return (void*)p;
}

void
pexit(char *msg, int freemem)
{
	Proc *p = up;

	USED(msg);
	USED(freemem);

	if(p->pgrp != nil){
		closepgrp(p->pgrp);
		p->pgrp = nil;
	}
	if(p->rgrp != nil){
		closergrp(p->rgrp);
		p->rgrp = nil;
	}
	if(p->fgrp != nil){
		closefgrp(p->fgrp);
		p->fgrp = nil;
	}

	cclose(p->dot);
	cclose(p->slash);

	free(p);
	osexit();
}
