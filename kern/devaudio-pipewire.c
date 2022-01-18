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
	Rendez z;
	int init;
	struct pw_main_loop *loop;
	struct pw_stream *output;

	char buf[8192];
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

	lock(&pwstate.lk);
	if(pwstate.written == 0){
		unlock(&pwstate.lk);
		return;
	}
	b = pw_stream_dequeue_buffer(pwstate.output);

	buf = b->buffer;
	dst = buf->datas[0].data;

	memcpy(dst, pwstate.buf, pwstate.written);
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = sizeof(int16_t) * 2;
	buf->datas[0].chunk->size = pwstate.written;

	pw_stream_queue_buffer(pwstate.output, b);
	pwstate.written = 0;
	unlock(&pwstate.lk);
	wakeup(&pwstate.z);
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
}

void
audiodevopen(void)
{
	const struct spa_pod *params[1];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pwstate.buf, sizeof(pwstate.buf));
	int err;

	lock(&pwstate.lk);
	if(pwstate.init > 0){
		kproc("pipewire main loop", pwproc, pwstate.loop);
		unlock(&pwstate.lk);
		return;
	}

	pwstate.init++;
	pw_init(&argc, (char***)&argv);
	pwstate.loop = pw_main_loop_new(NULL);
	if(pwstate.loop == NULL)
		sysfatal("could not create loop");
	pwstate.output = pw_stream_new_simple(
		pw_main_loop_get_loop(pwstate.loop),
		"drawterm",
		pw_properties_new(
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
	lock(&pwstate.lk);
	pw_main_loop_quit(pwstate.loop);
	unlock(&pwstate.lk);
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
	return pwstate.written == 0;
}

int
audiodevwrite(void *a, int n)
{
	if(n > sizeof(pwstate.buf)){
		error("write too large");
		return -1;
	}
	lock(&pwstate.lk);
	if(pwstate.written != 0){
		unlock(&pwstate.lk);
		sleep(&pwstate.z, canwrite, 0);
		lock(&pwstate.lk);
	}
	memcpy(pwstate.buf, a, n);
	pwstate.written = n;
	unlock(&pwstate.lk);
	return n;
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

