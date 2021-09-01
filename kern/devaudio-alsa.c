/*
 * ALSA
 */
#include <alsa/asoundlib.h>
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

static snd_pcm_t *playback;
static snd_pcm_t *capture;
static int speed = Rate;

/* maybe this should return -1 instead of sysfatal */
void
audiodevopen(void)
{
	if(snd_pcm_open(&playback, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		error("snd_pcm_open playback");

	if(snd_pcm_set_params(playback, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, speed, 1, 500000) < 0)
		error("snd_pcm_set_params playback");

	if(snd_pcm_prepare(playback) < 0)
		error("snd_pcm_prepare playback");

	if(snd_pcm_open(&capture, "default", SND_PCM_STREAM_CAPTURE, 0) < 0)
		error("snd_pcm_open capture");

	if(snd_pcm_set_params(capture, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, speed, 1, 500000) < 0)
		error("snd_pcm_set_params capture");

	if(snd_pcm_prepare(capture) < 0)
		error("snd_pcm_prepare capture");
}

void
audiodevclose(void)
{
	snd_pcm_drain(playback);
	snd_pcm_close(playback);

	snd_pcm_close(capture);
}

void
audiodevsetvol(int what, int left, int right)
{
	if(what == Vspeed){
		speed = left;
		return;
	}
}

void
audiodevgetvol(int what, int *left, int *right)
{
	if(what == Vspeed){
		*left = *right = speed;
		return;
	}

	*left = *right = 100;
}

int
audiodevwrite(void *v, int n)
{
	snd_pcm_sframes_t frames;
	int tot, m;

	for(tot = 0; tot < n; tot += m){
		do {
			frames = snd_pcm_writei(playback, v+tot, (n-tot)/4);
		} while(frames == -EAGAIN);
		if (frames < 0)
			frames = snd_pcm_recover(playback, frames, 0);
		if (frames < 0)
			error((char*)snd_strerror(frames));
		m = frames*4;
	}

	return tot;
}

int
audiodevread(void *v, int n)
{
	snd_pcm_sframes_t frames;

	do {
		frames = snd_pcm_readi(capture, v, n/4);
	} while(frames == -EAGAIN);

	if (frames < 0)
		error((char*)snd_strerror(frames));

	return frames*4;
}
