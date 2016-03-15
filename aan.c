#include <u.h>
#include <libc.h>
#include <fcall.h>

enum {
	Hdrsz = 3*4,
	Bufsize = 8*1024,
};

typedef struct Hdr Hdr;
typedef struct Buf Buf;
typedef struct Client Client;

struct Hdr {
	uchar	nb[4];		// Number of data bytes in this message
	uchar	msg[4];		// Message number
	uchar	acked[4];	// Number of messages acked
};

struct Buf {
	Hdr	hdr;
	uchar	buf[Bufsize];

	Buf	*next;
};

struct Client {
	QLock	lk;

	char	*addr;
	int	netfd;
	int	pipefd;

	int	reader;
	int	writer;
	int	syncer;

	long	inmsg;
	long	outmsg;

	Buf	*unackedhead;
	Buf	**unackedtail;
};

static void
reconnect(Client *c)
{
	Buf *b;
	int n;

	qlock(&c->lk);
Again:
	for(;;){
		if(c->netfd >= 0){
			close(c->netfd);
			c->netfd = -1;
		}
		if((c->netfd = dial(c->addr,nil,nil,nil)) >= 0)
			break;
		sleep(1000);
	}
	for(b = c->unackedhead; b != nil; b = b->next){
		n = GBIT32(b->hdr.nb);
		PBIT32(b->hdr.acked, c->inmsg);
		if(write(c->netfd, &b->hdr, Hdrsz) != Hdrsz
		|| write(c->netfd, b->buf, n) != n){
			print("write error: %r\n");
			goto Again;
		}
	}
	qunlock(&c->lk);
}

static void
aanwriter(void *arg)
{
	Client *c = (Client*)arg;
	Buf *b;
	int n;
	long m;

	for(;;){
		b = malloc(sizeof(Buf));
		if(b == nil)
			break;
		if((n = read(c->pipefd, b->buf, Bufsize)) < 0){
			free(b);
			break;
		}

		qlock(&c->lk);
		m = c->outmsg++;
		PBIT32(b->hdr.nb, n);
		PBIT32(b->hdr.msg, m);
		PBIT32(b->hdr.acked, c->inmsg);

		b->next = nil;
		if(c->unackedhead == nil)
			c->unackedtail = &c->unackedhead;
		*c->unackedtail = b;
		c->unackedtail = &b->next;

		if(c->netfd < 0
		|| write(c->netfd, &b->hdr, Hdrsz) != Hdrsz
		|| write(c->netfd, b->buf, n) != n){
			qunlock(&c->lk);
			break;
		}
		qunlock(&c->lk);

		if(n == 0)
			break;
	}
	close(c->pipefd);
	c->pipefd = -1;
}

static void
aansyncer(void *arg)
{
	Client *c = (Client*)arg;
	Hdr hdr;

	for(;;){
		sleep(4000);
		qlock(&c->lk);
		if(c->netfd >= 0){
			PBIT32(hdr.nb, 0);
			PBIT32(hdr.acked, c->inmsg);
			PBIT32(hdr.msg, -1);
			write(c->netfd, &hdr, Hdrsz);
		}
		qunlock(&c->lk);
	}
}

static void
aanreader(void *arg)
{
	Client *c = (Client*)arg;
	Buf *b, *x, **l;
	long a, m;
	int n;

Restart:
	b = mallocz(sizeof(Buf), 1);
	for(;;){
		if(readn(c->netfd, &b->hdr, Hdrsz) != Hdrsz)
			break;
		a = GBIT32(b->hdr.acked);
		m = GBIT32(b->hdr.msg);
		n = GBIT32(b->hdr.nb);
		if(n > Bufsize)
			break;

		qlock(&c->lk);
		l = &c->unackedhead;
		for(x = c->unackedhead; x != nil; x = *l){
			if(a >= GBIT32(x->hdr.msg)){
				if((*l = x->next) == nil)
					c->unackedtail = l;
				free(x);
			} else {
				l = &x->next;
			}
		}
		qunlock(&c->lk);

		if(readn(c->netfd, b->buf, n) != n)
			break;
		if(m < c->inmsg)
			continue;
		c->inmsg++;
		if(c->pipefd < 0)
			return;
		write(c->pipefd, b->buf, n);
	}
	free(b);
	reconnect(c);
	goto Restart;
}

int
aanclient(char *addr)
{
	Client *c;
	int pfd[2];

	if(pipe(pfd) < 0)
		sysfatal("pipe: %r");

	c = mallocz(sizeof(Client), 1);
	c->addr = addr;
	c->netfd = -1;
	c->pipefd = pfd[1];
	c->inmsg = 0;
	c->outmsg = 0;
	reconnect(c);
	c->writer = kproc("aanwriter", aanwriter, c);
	c->reader = kproc("aanreader", aanreader, c);
	c->syncer = kproc("aansyncer", aansyncer, c);
	return pfd[0];
}
