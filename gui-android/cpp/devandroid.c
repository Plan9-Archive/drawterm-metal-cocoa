#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

#include <android/log.h>
#include <android/sensor.h>

void show_notification(char *buf);
void take_picture(int id);
int num_cameras();

int Ncameras = 0;

uchar *cambuf = nil;
int camlen;

ASensorManager *sensorManager = NULL;

enum
{
	Qdir		= 0,
	Qcam		= 1,
	Qaccel		= 2,
	Qcompass	= 4,
	Qnotification	= 6,
};
#define QID(p, c, y) 	(((p)<<16) | ((c)<<4) | (y))

static void androidinit(void);

static void
androidinit(void)
{
	sensorManager = ASensorManager_getInstance();

	Ncameras = num_cameras();
}

static Chan*
androidattach(char *param)
{
	Chan *c;

	c = devattach('N', param);
	c->qid.path = QID(0, 0, Qdir);
	c->qid.type = QTDIR;
	c->qid.vers = 0;

	return c;
}

static int
androidgen(Chan *c, char *n, Dirtab *d, int nd, int s, Dir *dp)
{
	Qid q;

	if (s == DEVDOTDOT) {
		mkqid(&q, Qdir, 0, QTDIR);
		devdir(c, q, "#N", 0, eve, 0555, dp);
		return 1;
	}
	if (s < Ncameras) {
		sprintf(up->genbuf, "cam%d.jpg", s);
		mkqid(&q, (s << 16) | Qcam, 0, QTFILE);
		devdir(c, q, up->genbuf, 0, eve, 0444, dp);
		return 1;
	}
	if (s == Ncameras) {
		sprintf(up->genbuf, "accel");
		mkqid(&q, Qaccel, 0, QTFILE);
		devdir(c, q, up->genbuf, 0, eve, 0444, dp);
		return 1;
	}
	if (s == (Ncameras+1)) {
		sprintf(up->genbuf, "compass");
		mkqid(&q, Qcompass, 0, QTFILE);
		devdir(c, q, up->genbuf, 0, eve, 0444, dp);
		return 1;
	}
	if (s == (Ncameras+2)) {
		sprintf(up->genbuf, "notification");
		mkqid(&q, Qnotification, 0, QTFILE);
		devdir(c, q, up->genbuf, 0, eve, 0222, dp);
		return 1;
	}
	return -1;
}

static Walkqid*
androidwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, 0, 0, androidgen);
}

static int
androidstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, 0, 0, androidgen);
}

static Chan*
androidopen(Chan *c, int omode)
{
	p9_uvlong s;

	c = devopen(c, omode, 0, 0, androidgen);

	if (c->qid.path & Qcam) {
		s = c->qid.path >> 16;
		take_picture(s);
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = 8192;

	return c;
}

static void
androidclose(Chan *c)
{
	if (c->qid.path & Qcam && cambuf != nil) {
		free(cambuf);
		cambuf = nil;
	}
}

static long
androidread(Chan *c, void *v, long n, vlong off)
{
	char *a = v;
	long l;
	const ASensor *sensor;
	ASensorEventQueue *queue = NULL;
	ASensorEvent data;

	switch((ulong)c->qid.path & 0xF) {
		default:
			error(Eperm);
			return -1;

		case Qcam:
			while(cambuf == nil)
				usleep(10 * 1000);

			l = camlen - off;
			if (l > n)
				l = n;

			if (l > 0)
				memcpy(a, cambuf + off, l);

			return l;
		case Qaccel:
			queue = ASensorManager_createEventQueue(sensorManager, ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS), 1, NULL, NULL);
			if (queue == NULL)
				return 0;
			sensor = ASensorManager_getDefaultSensor(sensorManager, ASENSOR_TYPE_ACCELEROMETER);
			if (sensor == NULL) {
				ASensorManager_destroyEventQueue(sensorManager, queue);
				return 0;
			}
			if (ASensorEventQueue_enableSensor(queue, sensor)) {
				ASensorEventQueue_disableSensor(queue, sensor);
				ASensorManager_destroyEventQueue(sensorManager, queue);
				return 0;
			}
			l = 0;
			if (ALooper_pollAll(1000, NULL, NULL, NULL) == 1) {
				if (ASensorEventQueue_getEvents(queue, &data, 1)) {
					l = snprint(a, n, "%11f %11f %11f\n", data.vector.x, data.vector.y, data.vector.z);
				}
			}
			ASensorEventQueue_disableSensor(queue, sensor);
			ASensorManager_destroyEventQueue(sensorManager, queue);
			return l;
		case Qcompass:
			queue = ASensorManager_createEventQueue(sensorManager, ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS), 1, NULL, NULL);
			if (queue == NULL)
				return 0;
			sensor = ASensorManager_getDefaultSensor(sensorManager, ASENSOR_TYPE_MAGNETIC_FIELD);
			if (sensor == NULL) {
				ASensorManager_destroyEventQueue(sensorManager, queue);
				return 0;
			}
			if (ASensorEventQueue_enableSensor(queue, sensor)) {
				ASensorEventQueue_disableSensor(queue, sensor);
				ASensorManager_destroyEventQueue(sensorManager, queue);
				return 0;
			}
			l = 0;
			if (ALooper_pollAll(1000, NULL, NULL, NULL) == 1) {
				if (ASensorEventQueue_getEvents(queue, &data, 1)) {
					l = snprint(a, n, "%11f %11f %11f\n", data.vector.x, data.vector.y, data.vector.z);
				}
			}
			ASensorEventQueue_disableSensor(queue, sensor);
			ASensorManager_destroyEventQueue(sensorManager, queue);
			return l;
		case Qdir:
			return devdirread(c, a, n, 0, 0, androidgen);
	}
}

static long
androidwrite(Chan *c, void *vp, long n, vlong off)
{
	char *a = vp;
	char *str;

	switch((ulong)c->qid.path) {
		case Qnotification:
			str = malloc(n+1);
			memcpy(str, a, n);
			str[n] = '\0';
			show_notification(str);
			free(str);
			return n;
		default:
			error(Eperm);
			break;
	}
	return -1;
}

Dev androiddevtab = {
	'N',
	"android",

	devreset,
	androidinit,
	devshutdown,
	androidattach,
	androidwalk,
	androidstat,
	androidopen,
	devcreate,
	androidclose,
	androidread,
	devbread,
	androidwrite,
	devbwrite,
	devremove,
	devwstat,
};
