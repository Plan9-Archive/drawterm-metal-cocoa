#include	<sndio.h>

#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"
#include	"devaudio.h"

enum
{
	Channels = 2,
	Rate = 44100,
	Bits = 16,
};

static struct sio_hdl *hdl;
static struct sio_par par;

void
audiodevopen(void)
{
	hdl = sio_open(SIO_DEVANY, SIO_PLAY, 0);
	if(hdl == NULL){
		error("sio_open failed");
		return;
	}

	sio_initpar(&par);

	par.bits = Bits;
	par.pchan = Channels;
	par.rate = Rate;
	par.appbufsz = 288000;

	if(!sio_setpar(hdl, &par) || !sio_start(hdl)){
		sio_close(hdl);
		error("sio_setpar/sio_start failed");
		return;
	}
}

void
audiodevclose(void)
{
	sio_close(hdl);
}

void
audiodevsetvol(int what, int left, int right)
{
	USED(what);
	USED(left);
	USED(right);
	error("not supported");
}

void
audiodevgetvol(int what, int *left, int *right)
{
	USED(what);
	USED(left);
	USED(right);
	error("not supported");
}

int
audiodevwrite(void *v, int n)
{
	return sio_write(hdl, v, n);
}

int
audiodevread(void *v, int n)
{
	error("no reading");
	return -1;
}
