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
	int	timeout;

	ulong	inmsg;
	ulong	outmsg;

	Buf	*unackedhead;
	Buf	**unackedtail;
};

static void
reconnect(Client *c)
{
	Buf *b;
	int n;
	ulong to;

	qlock(&c->lk);
	to = (ulong)time(0) + c->timeout;
Again:
	for(;;){
		if(c->netfd >= 0){
			close(c->netfd);
			c->netfd = -1;
		}
		if((c->netfd = dial(c->addr,nil,nil,nil)) >= 0)
			break;		
		if((ulong)time(0) >= to)
			sysfatal("dial timed out: %r");
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
	ulong m;

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
			continue;
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
	ulong a, m, lastacked = 0;
	Buf *b, *x;
	int n;

Restart:
	b = mallocz(sizeof(Buf), 1);
	for(;;){
		if(readn(c->netfd, &b->hdr, Hdrsz) != Hdrsz)
			break;
		a = GBIT32(b->hdr.acked);
		m = GBIT32(b->hdr.msg);
		n = GBIT32(b->hdr.nb);
		if(n == 0){
			if(m == (ulong)-1)
				continue;
			goto Closed;
		} else if(n < 0 || n > Bufsize)
			goto Closed;

		if(readn(c->netfd, b->buf, n) != n)
			break;
		if(m != c->inmsg)
			continue;
		c->inmsg++;

		if((long)(a - lastacked) > 0){
			qlock(&c->lk);
			while((x = c->unackedhead) != nil){
				assert(GBIT32(x->hdr.msg) == lastacked);
				c->unackedhead = x->next;
				free(x);
				if(++lastacked == a)
					break;
			}
			qunlock(&c->lk);
		}

		if(c->pipefd < 0)
			goto Closed;
		write(c->pipefd, b->buf, n);
	}
	free(b);
	reconnect(c);
	goto Restart;
Closed:
	free(b);
	if(c->pipefd >= 0)
		write(c->pipefd, "", 0);
}

int
aanclient(char *addr, int timeout)
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
	c->timeout = 60;
	reconnect(c);
	c->timeout = timeout;
	kproc("aanwriter", aanwriter, c);
	kproc("aanreader", aanreader, c);
	kproc("aansyncer", aansyncer, c);
	return pfd[0];
}
