#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"
#include	"devaudio.h"

#undef long
#undef ulong
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

static struct {
	Lock lk;
	Rendez w;
	int init;
	struct pw_main_loop *loop;
	struct pw_stream *output;

	char buf[2*2*44100/10]; /* 1/10th sec */
	int written; /* 0 means empty buffer */
} pwstate;

static char *argv[] = { "drawterm" };
static int argc = 1;

static void
on_process(void *data)
{
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int16_t *dst;
	int n;

	lock(&pwstate.lk);
	if(pwstate.written == sizeof(pwstate.buf))
		wakeup(&pwstate.w);
	if(pwstate.written == 0){
		unlock(&pwstate.lk);
		return;
	}

	if((b = pw_stream_dequeue_buffer(pwstate.output)) == nil)
		return;
	buf = b->buffer;
	dst = buf->datas[0].data;

	n = pwstate.written;
	if(n > buf->datas[0].maxsize)
		n = buf->datas[0].maxsize;
	memcpy(dst, pwstate.buf, n);
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = sizeof(int16_t) * 2;
	buf->datas[0].chunk->size = n;
	pwstate.written -= n;
	if(pwstate.written > 0)
		memmove(pwstate.buf, pwstate.buf+n, pwstate.written);

	pw_stream_queue_buffer(pwstate.output, b);
	unlock(&pwstate.lk);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void
pwproc(void *arg)
{
	struct pw_main_loop *loop;

	loop = arg;
	pw_main_loop_run(loop);
	pexit("", 0);
}

void
audiodevopen(void)
{
	const struct spa_pod *params[1];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pwstate.buf, sizeof(pwstate.buf));
	int err;

	lock(&pwstate.lk);
	pwstate.written = 0;
	if(pwstate.init == 0){
		pw_init(&argc, (char***)&argv);
		pwstate.init++;
		pwstate.loop = pw_main_loop_new(NULL);
		if(pwstate.loop == NULL)
			sysfatal("could not create loop");
	}

	pwstate.output = pw_stream_new_simple(
		pw_main_loop_get_loop(pwstate.loop),
		"drawterm",
		pw_properties_new(
			PW_KEY_NODE_NAME, "drawterm",
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Music",
			NULL),
		&stream_events,
		NULL);

	if(pwstate.output == NULL){
		unlock(&pwstate.lk);
		error("could not create pipewire output");
		return;
	}
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_S16_LE,
			.channels = 2,
			.rate = 44100 ));

	err = pw_stream_connect(pwstate.output,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT |
		PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS,
		params, 1);

	unlock(&pwstate.lk);
	if(err < 0){
		error("could not connect pipewire stream");
		return;
	}

	kproc("pipewire main loop", pwproc, pwstate.loop);
}

void
audiodevclose(void)
{
	pw_main_loop_quit(pwstate.loop);
	pw_stream_destroy(pwstate.output);
}

int
audiodevread(void *a, int n)
{
	error("no record support");
	return -1;
}

static int
canwrite(void *arg)
{
	return pwstate.written < sizeof(pwstate.buf);
}

int
audiodevwrite(void *a, int n)
{
	int w, x, max;
	char *p;

	w = n;
	for(p = a; n > 0; p += x, n -= x){
		lock(&pwstate.lk);
		max = sizeof(pwstate.buf) - pwstate.written;
		x = n > max ? max : n;
		if(x < 1){
			unlock(&pwstate.lk);
			sleep(&pwstate.w, canwrite, 0);
		}else{
			memmove(pwstate.buf+pwstate.written, p, x);
			pwstate.written += x;
			unlock(&pwstate.lk);
		}
	}
	return w;
}

void
audiodevsetvol(int what, int left, int right)
{
	error("no volume support");
}

void
audiodevgetvol(int what, int *left, int *right)
{
	error("no volume support");
}

