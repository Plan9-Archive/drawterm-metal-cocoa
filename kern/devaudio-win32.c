#include	<windows.h>
#include	<mmsystem.h>

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

typedef struct Waveblock Waveblock;
struct Waveblock {
	WAVEHDR	h;
	uchar	s[2048];
};

static HWAVEOUT waveout;
static Waveblock blk[16];
static uint blkidx;

void
audiodevopen(void)
{
	WAVEFORMATEX f;

	memset(&f, 0, sizeof(f));
	f.nSamplesPerSec = Rate;
	f.wBitsPerSample = Bits;
	f.nChannels = Channels;
	f.cbSize = 0;
	f.wFormatTag = WAVE_FORMAT_PCM;
	f.nBlockAlign = (f.wBitsPerSample/8) * f.nChannels;
	f.nAvgBytesPerSec = f.nBlockAlign * f.nSamplesPerSec;
	if(waveOutOpen(&waveout, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
		oserror();
}

void
audiodevclose(void)
{
	waveOutClose(waveout);
	waveout = 0;
}

void
audiodevsetvol(int what, int left, int right)
{
	DWORD v;

	//Windows uses a 0-0xFFFF scale, plan9 uses 0-100
	v = right*0xFFFF/100;
	v = (v<<16)|(left*0xFFFF/100);
	if(waveOutSetVolume(waveout, v) != MMSYSERR_NOERROR)
		oserror();
}

void
audiodevgetvol(int what, int *left, int *right)
{
	DWORD v;

	if(waveOutGetVolume(waveout, &v) != MMSYSERR_NOERROR)
		oserror();
	*left = (v&0xFFFF)*100/0xFFFF;
	*right = ((v>>16)&0xFFFF)*100/0xFFFF;
}

int
audiodevwrite(void *v, int n)
{
	Waveblock *b;
	int m;

	m = 0;
	while(n > sizeof(b->s)){
		audiodevwrite(v, sizeof(b->s));
		v = (uchar*)v + sizeof(b->s);
		n -= sizeof(b->s);
		m += sizeof(b->s);
	}

	b = &blk[blkidx++ % nelem(blk)];
	if(b->h.dwFlags & WHDR_PREPARED){
		while(waveOutUnprepareHeader(waveout, &b->h, sizeof(b->h)) == WAVERR_STILLPLAYING)
			osmsleep(50);
	}
	memmove(b->s, v, n);
	b->h.lpData = (void*)b->s;
	b->h.dwBufferLength = n;
	waveOutPrepareHeader(waveout, &b->h, sizeof(b->h));
	waveOutWrite(waveout, &b->h, sizeof(b->h));

	return m + n;
}

int
audiodevread(void *v, int n)
{
	USED(v);
	USED(n);

	error("no reading");
	return -1;
}
