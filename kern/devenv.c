#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

enum
{
	Maxenvsize = 16300,
};

static Egrp	*envgrp(Chan *c);
static int	envwriteable(Chan *c);
static void	initunix();

static Egrp	unixegrp;	/* unix environment group */

static Evalue*
envlookup(Egrp *eg, char *name, ulong qidpath)
{
	Evalue *e;
	int i;

	for(i=0; i<eg->nent; i++){
		e = eg->ent[i];
		if(e->qid.path == qidpath || (name && e->name[0]==name[0] && strcmp(e->name, name) == 0))
			return e;
	}
	return nil;
}

static int
envgen(Chan *c, char *name, Dirtab *_1, int _2, int s, Dir *dp)
{
	Egrp *eg;
	Evalue *e;

	if(s == DEVDOTDOT){
		devdir(c, c->qid, "#e", 0, eve, DMDIR|0775, dp);
		return 1;
	}

	eg = envgrp(c);
	rlock(&eg->lk);
	e = 0;
	if(name)
		e = envlookup(eg, name, -1);
	else if(s < eg->nent)
		e = eg->ent[s];

	if(e == 0) {
		runlock(&eg->lk);
		return -1;
	}

	/* make sure name string continues to exist after we release lock */
	kstrcpy(up->genbuf, e->name, sizeof up->genbuf);
	devdir(c, e->qid, up->genbuf, e->len, eve, 0666, dp);
	runlock(&eg->lk);
	return 1;
}

static Chan*
envattach(char *spec)
{
	Chan *c;

	if(spec && *spec) {
		error(Ebadarg);
	}
	initunix();
	c = devattach('e', spec);
	c->aux = &unixegrp;
	return c;
}

static Walkqid*
envwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, 0, 0, envgen);
}

static int
envstat(Chan *c, uchar *db, int n)
{
	if(c->qid.type & QTDIR)
		c->qid.vers = envgrp(c)->vers;
	return devstat(c, db, n, 0, 0, envgen);
}

static Chan*
envopen(Chan *c, int omode)
{
	Egrp *eg;
	Evalue *e;
	int trunc;

	eg = envgrp(c);
	if(c->qid.type & QTDIR) {
		if(omode != OREAD)
			error(Eperm);
	}
	else {
		trunc = omode & OTRUNC;
		if(omode != OREAD && !envwriteable(c))
			error(Eperm);
		if(trunc)
			wlock(&eg->lk);
		else
			rlock(&eg->lk);
		e = envlookup(eg, nil, c->qid.path);
		if(e == 0) {
			if(trunc)
				wunlock(&eg->lk);
			else
				runlock(&eg->lk);
			error(Enonexist);
		}
		if(trunc && e->value) {
			e->qid.vers++;
			free(e->value);
			e->value = 0;
			e->len = 0;
		}
		if(trunc)
			wunlock(&eg->lk);
		else
			runlock(&eg->lk);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static Chan*
envcreate(Chan *c, char *name, int omode, ulong _)
{
	Egrp *eg;
	Evalue *e;
	Evalue **ent;

	if(c->qid.type != QTDIR)
		error(Eperm);
	if(strlen(name) >= sizeof up->genbuf)
		error("name too long");			/* protect envgen */

	omode = openmode(omode);
	eg = envgrp(c);

	wlock(&eg->lk);
	if(waserror()) {
		wunlock(&eg->lk);
		nexterror();
	}

	if(envlookup(eg, name, -1))
		error(Eexist);

	e = smalloc(sizeof(Evalue));
	e->name = smalloc(strlen(name)+1);
	strcpy(e->name, name);

	if(eg->nent == eg->ment){
		eg->ment += 32;
		ent = smalloc(sizeof(eg->ent[0])*eg->ment);
		if(eg->nent)
			memmove(ent, eg->ent, sizeof(eg->ent[0])*eg->nent);
		free(eg->ent);
		eg->ent = ent;
	}
	e->qid.path = ++eg->path;
	e->qid.vers = 0;
	eg->vers++;
	eg->ent[eg->nent++] = e;
	c->qid = e->qid;

	wunlock(&eg->lk);
	poperror();

	c->offset = 0;
	c->mode = omode;
	c->flag |= COPEN;
	return c;
}

static void
envremove(Chan *c)
{
	int i;
	Egrp *eg;
	Evalue *e;

	if(c->qid.type & QTDIR)
		error(Eperm);

	eg = envgrp(c);
	wlock(&eg->lk);
	e = 0;
	for(i=0; i<eg->nent; i++){
		if(eg->ent[i]->qid.path == c->qid.path){
			e = eg->ent[i];
			eg->nent--;
			eg->ent[i] = eg->ent[eg->nent];
			eg->vers++;
			break;
		}
	}
	wunlock(&eg->lk);
	if(e == 0)
		error(Enonexist);
	free(e->name);
	if(e->value)
		free(e->value);
	free(e);
}

static void
envclose(Chan *c)
{
	/*
	 * cclose can't fail, so errors from remove will be ignored.
	 * since permissions aren't checked,
	 * envremove can't not remove it if its there.
	 */
	if(c->flag & CRCLOSE)
		envremove(c);
}

static long
envread(Chan *c, void *a, long n, vlong off)
{
	Egrp *eg;
	Evalue *e;
	ulong offset = off;

	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, 0, 0, envgen);

	eg = envgrp(c);
	rlock(&eg->lk);
	e = envlookup(eg, nil, c->qid.path);
	if(e == 0) {
		runlock(&eg->lk);
		error(Enonexist);
	}

	if(offset > e->len)	/* protects against overflow converting vlong to ulong */
		n = 0;
	else if(offset + n > e->len)
		n = e->len - offset;
	if(n <= 0)
		n = 0;
	else
		memmove(a, e->value+offset, n);
	runlock(&eg->lk);
	return n;
}

static long
envwrite(Chan *c, void *a, long n, vlong off)
{
	char *s;
	ulong len;
	Egrp *eg;
	Evalue *e;
	ulong offset = off;

	if(n <= 0)
		return 0;
	if(offset > Maxenvsize || n > (Maxenvsize - offset))
		error(Etoobig);

	eg = envgrp(c);
	wlock(&eg->lk);
	e = envlookup(eg, nil, c->qid.path);
	if(e == 0) {
		wunlock(&eg->lk);
		error(Enonexist);
	}

	len = offset+n;
	if(len > e->len) {
		s = smalloc(len);
		if(e->value){
			memmove(s, e->value, e->len);
			free(e->value);
		}
		e->value = s;
		e->len = len;
	}
	memmove(e->value+offset, a, n);
	e->qid.vers++;
	eg->vers++;
	wunlock(&eg->lk);
	return n;
}

Dev envdevtab = {
	'e',
	"env",

	devreset,
	devinit,
	devshutdown,
	envattach,
	envwalk,
	envstat,
	envopen,
	envcreate,
	envclose,
	envread,
	devbread,
	envwrite,
	devbwrite,
	envremove,
	devwstat,
};

extern char **environ;

static void
initunix()
{
	Egrp *eg = &unixegrp;
	Evalue **ent, *e;
	char *eq, **envp, *line;
	int n;

	wlock(&eg->lk);

	if(eg->path > 0 || eg->ment > 0 || !environ){
		// already initialized or nothing in environent
		wunlock(&eg->lk);
		return;
	}

	for(envp = environ; *envp != nil; envp++)
		eg->ment++;
	ent = smalloc(sizeof(eg->ent[0])*eg->ment);
	eg->ent = ent;

	for(envp = environ; *envp != nil; envp++){
		line = *envp;
		n = strlen(line);

		eq = strchr(line, '=');
		if(eq == nil)
			eq = &line[n];
		e = smalloc(sizeof(Evalue));
		e->name = smalloc(eq-line+1);
		strncpy(e->name, line, eq-line);

		if(eq[0] != '\0')
			eq++;
		e->len = line+n-eq;
		e->value = smalloc(e->len);
		memmove(e->value, eq, e->len);

		e->qid.path = ++eg->path;
		e->qid.vers = 0;
		eg->vers++;
		eg->ent[eg->nent++] = e;
	}

	wunlock(&eg->lk);
}

void
envcpy(Egrp *to, Egrp *from)
{
	int i;
	Evalue *ne, *e;

	rlock(&from->lk);
	to->ment = (from->nent+31)&~31;
	to->ent = smalloc(to->ment*sizeof(to->ent[0]));
	for(i=0; i<from->nent; i++){
		e = from->ent[i];
		ne = smalloc(sizeof(Evalue));
		ne->name = smalloc(strlen(e->name)+1);
		strcpy(ne->name, e->name);
		if(e->value){
			ne->value = smalloc(e->len);
			memmove(ne->value, e->value, e->len);
			ne->len = e->len;
		}
		ne->qid.path = ++to->path;
		to->ent[i] = ne;
	}
	to->nent = from->nent;
	runlock(&from->lk);
}

void
closeegrp(Egrp *eg)
{
	int i;
	Evalue *e;

	if(decref(&eg->ref) == 0){
		for(i=0; i<eg->nent; i++){
			e = eg->ent[i];
			free(e->name);
			if(e->value)
				free(e->value);
			free(e);
		}
		free(eg->ent);
		free(eg);
	}
}

static Egrp*
envgrp(Chan *c)
{
	if(c->aux == nil)
		return &unixegrp;
	return c->aux;
}

static int
envwriteable(Chan *c)
{
	return iseve() || c->aux == nil;
}
