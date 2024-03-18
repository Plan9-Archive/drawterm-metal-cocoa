#include <Autolock.h>
#include <SoundPlayer.h>
#include <memory>

extern "C" {
#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"
#include "devaudio.h"
}

static struct {
  BLocker locker;
  std::unique_ptr<BSoundPlayer> player;
  uint8 buffer[44100 * 4 / 10];
  size_t written;
} snd;

static void play(void *cookie, void *buffer, size_t size,
                 const media_raw_audio_format &format) {
  BAutolock locker(&snd.locker);
  if (snd.written >= size) {
    memcpy(buffer, snd.buffer, size);
    snd.written -= size;
    if (snd.written > 0)
      memmove(snd.buffer, snd.buffer + size, snd.written);
  } else {
    memset(buffer, 0, size);
  }
}

void audiodevopen(void) {
  const media_raw_audio_format format = {
      .frame_rate = 44100,
      .channel_count = 2,
      .format = media_raw_audio_format::B_AUDIO_SHORT,
      .byte_order = B_MEDIA_LITTLE_ENDIAN,
      .buffer_size = 4096,
  };
  snd.player = std::make_unique<BSoundPlayer>(&format, "devaudio", play);
  snd.written = 0;
  if (snd.player->InitCheck() != B_OK) {
    snd.player.reset(nullptr);
    return;
  }
  snd.player->Start();
}

void audiodevclose(void) { snd.player.reset(nullptr); }

int audiodevread(void *a, int n) {
  error("no reading");
  return -1;
}

int audiodevwrite(void *a, int n) {
  int w = n;
  uint8 *p = (uint8 *)a;

  if (snd.player.get() == nullptr)
    return -1;

  while (n > 0) {
    snd.locker.Lock();
    int max = sizeof(snd.buffer) - snd.written;
    int x = n > max ? max : n;
    if (x < 1) {
      snd.locker.Unlock();
      osmsleep(10);
    } else {
      memcpy(snd.buffer + snd.written, p, x);
      snd.written += x;
      p += x;
      n -= x;
      snd.locker.Unlock();
    }
  }
  return w;
}

void audiodevsetvol(int what, int left, int right) { error("not supported"); }

void audiodevgetvol(int what, int *left, int *right) { error("not supported"); }
