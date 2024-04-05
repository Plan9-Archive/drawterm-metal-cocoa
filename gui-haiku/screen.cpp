#include <memory>
#include <Autolock.h>
#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <Screen.h>
#include <Window.h>
#include <Input.h>
#include <private/input/InputServerTypes.h>
#include <private/interface/input_globals.h>

extern "C" {
#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"
#include "devaudio.h"
#include "draw.h"
#include "memdraw.h"
#include "cursor.h"
#include "keyboard.h"
#include "screen.h"
}

Memimage *gscreen = (Memimage *)nil;

class View : public BView {
private:
  std::unique_ptr<BBitmap> bitmap;
  Mousestate ms;

public:
  View(BRect frame)
      : BView(frame, NULL, B_FOLLOW_ALL_SIDES,
              B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE) {
    SetViewColor(B_TRANSPARENT_COLOR);
    bitmap = std::make_unique<BBitmap>(frame, B_RGBA32);
  }

  void *GetFrameBuffer() { return bitmap->Bits(); }

  void Draw(BRect updateRect) override {
    qlock(&drawlock);
    DrawBitmap(bitmap.get(), updateRect, updateRect);
    qunlock(&drawlock);
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case B_VIEW_RESIZED: {
      int32 width, height;
      width = message->GetInt32("width", 0);
      height = message->GetInt32("height", 0);
      qlock(&drawlock);
      bitmap = std::make_unique<BBitmap>(BRect(0, 0, width, height), B_RGBA32);
      memset(bitmap->Bits(), 0xff, bitmap->BitsLength());
      qunlock(&drawlock);
      screenresize(Rect(0, 0, width + 1, height + 1));
      break;
    }

    case B_KEY_DOWN:
    case B_UNMAPPED_KEY_DOWN:
    case B_KEY_UP:
    case B_UNMAPPED_KEY_UP: {
      static struct {
         int8 byte;
         int32 key;
         Rune k;
      } transtab[] = {
         0x00, 0x3b, Kcaps,
         0x00, 0x5C, Kctl,
         0x00, 0x60, Kctl,
         0x00, 0x4b, Kshift,
         0x00, 0x56, Kshift,
         0x00, 0x5d, Kalt,
         0x00, 0x5f, Kalt,
         0x00, 0x66, Kmod4,

         0x10, B_F1_KEY, KF | 1,
         0x10, B_F2_KEY, KF | 2,
         0x10, B_F3_KEY, KF | 3,
         0x10, B_F4_KEY, KF | 4,
         0x10, B_F5_KEY, KF | 5,
         0x10, B_F6_KEY, KF | 6,
         0x10, B_F7_KEY, KF | 7,
         0x10, B_F8_KEY, KF | 8,
         0x10, B_F9_KEY, KF | 9,
         0x10, B_F10_KEY, KF | 10,
         0x10, B_F11_KEY, KF | 11,
         0x10, B_F12_KEY, KF | 12,
         0x10, B_PRINT_KEY, Kprint,
         0x10, B_SCROLL_KEY, Kscroll,
         0x10, B_PAUSE_KEY, Kbreak,

         B_DELETE, 0x34, Kdel,
         B_INSERT, 0x1f, Kins,
         B_HOME, 0x20, Khome,
         B_END, 0x35, Kend,
         B_PAGE_UP, 0x21, Kpgup,
         B_PAGE_DOWN, 0x36, Kpgdown,
         B_UP_ARROW, 0x57, Kup,
         B_RIGHT_ARROW, 0x63, Kright,
         B_LEFT_ARROW, 0x61, Kleft,
         B_DOWN_ARROW, 0x62, Kdown,
      };
      int32 key = message->GetInt32("key", 0);
      int8 byte = message->GetInt8("byte", 0);
      bool down =
          message->what == B_KEY_DOWN || message->what == B_UNMAPPED_KEY_DOWN;
      Rune k = 0;
      for (int i = 0; i < nelem(transtab); i++){
        if (key != transtab[i].key || byte != transtab[i].byte)
          continue;
        k = transtab[i].k;
        break;
      }
      kbdkey(k == 0 ? byte : k, down);
      break;
    }

    case B_MOUSE_MOVED: {
      BPoint where = message->GetPoint("where", BPoint(0, 0));
      ms.xy.x = where.x;
      ms.xy.y = where.y;
      ms.msec = message->GetInt64("when", 0) / 1000;
      absmousetrack(ms.xy.x, ms.xy.y, ms.buttons, ms.msec);
      break;
    }

    case B_MOUSE_WHEEL_CHANGED: {
      float delta_y = message->GetFloat("be:wheel_delta_y", 0);
      int32 buttons = delta_y < 0 ? 8 : 16;
      ms.msec = message->GetInt64("when", 0) / 1000;
      absmousetrack(ms.xy.x, ms.xy.y, buttons, ms.msec);
      break;
    }

    case B_MOUSE_DOWN:
    case B_MOUSE_UP: {
      int32 buttons = message->GetInt32("buttons", 0);
      ms.msec = message->GetInt64("when", 0) / 1000;
      ms.buttons = 0;
      if (buttons & B_MOUSE_BUTTON(1))
        ms.buttons |= 1;
      if (buttons & B_MOUSE_BUTTON(2))
        ms.buttons |= 4;
      if (buttons & B_MOUSE_BUTTON(3))
        ms.buttons |= 2;
      absmousetrack(ms.xy.x, ms.xy.y, ms.buttons, ms.msec);
      break;
    }

    default:
      BView::MessageReceived(message);
    }
  }
};

static void winproc(void *arg) {
  BWindow *window = (BWindow *)arg;
  thread_id tid = find_thread(NULL);
  rename_thread(tid, "window");
  BAutolock locker(window);
  window->Loop();
}

static const uint32 kToggleFullScreen = 'TFSn';

class Window : public BWindow {
private:
  BRect fSavedFrame;
  bool fFullScreen;

  void _ToggleFullScreen() {
    if (fFullScreen) {
      ResizeTo(fSavedFrame.Width(), fSavedFrame.Height());
      MoveTo(fSavedFrame.left, fSavedFrame.top);
      SetFlags(Flags() & ~(B_NOT_RESIZABLE | B_NOT_MOVABLE));
      fFullScreen = false;
    } else {
      BScreen screen(this);
      BRect frame = screen.Frame();
      fSavedFrame = Frame();
      ResizeTo(frame.Width() + 1, frame.Height() + 1);
      MoveTo(frame.left, frame.top);
      SetFlags(Flags() | (B_NOT_RESIZABLE | B_NOT_MOVABLE));
      fFullScreen = true;
    }
  }

public:
  Window(BRect frame)
      : BWindow(frame, "drawterm", B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE) {
    fFullScreen = false;

    BMessage *message = new BMessage(kToggleFullScreen);
    AddShortcut(B_ENTER, B_COMMAND_KEY, message);
  }

  ~Window() {
    status_t exit_value;
    wait_for_thread(Thread(), &exit_value);
  }

  thread_id Run() override {
    thread_id tid = 0;
    if (IsLocked()) {
      kproc("window", winproc, this);
      while (tid == 0) {
        tid = find_thread("window");
        osmsleep(10);
      }
      Unlock();
    }
    return tid;
  }

  bool QuitRequested() override {
    status_t exit_value;
    thread_id tid = find_thread("cpu");
    audiodevclose();
    kill_thread(tid);
    wait_for_thread(tid, &exit_value);
    return BLooper::QuitRequested();
  }

  void MessageReceived(BMessage *message) override {
    switch (message->what) {
    case kToggleFullScreen:
      _ToggleFullScreen();
      break;
    default:
      BWindow::MessageReceived(message);
    }
  }
};

class Application : public BApplication {
public:
  Window *window;
  View *view;

  Application() : BApplication("application/x-vnd.9front-drawterm") {
    BRect frame(0, 0, 640 - 1, 480 - 1);
    window = new Window(frame);
    view = new View(frame);
    window->AddChild(view);
    window->CenterOnScreen();
    view->MakeFocus();
    window->Show();
  }

  bool QuitRequested() override {
    BMessage message(B_QUIT_REQUESTED);
    window->PostMessage(&message);
    return true;
  }
};
#define sApp ((Application *)be_app)

char *clipread(void) {
  const void *data = NULL;
  char *ret = NULL;
  ssize_t textLen;
  if (be_clipboard->Lock()) {
    BMessage *clip = be_clipboard->Data();
    if (clip){
      clip->FindData("text/plain", B_MIME_TYPE, &data, &textLen);
      ret = (char*)malloc(textLen + 1);
      if (ret != NULL){
        memcpy(ret, data, textLen);
        ret[textLen] = '\0';
      }
    }
    be_clipboard->Unlock();
  }
  return ret;
}

int clipwrite(char *text) {
  ssize_t textLen = strlen(text);
  if (be_clipboard->Lock()) {
    BMessage *clip = be_clipboard->Data();
    if (clip) {
      be_clipboard->Clear();
      clip->AddData("text/plain", B_MIME_TYPE, text, textLen);
      be_clipboard->Commit();
    }
    be_clipboard->Unlock();
  }
  return textLen;
}

void flushmemscreen(Rectangle r) {
  BRect rect(r.min.x, r.min.y, r.max.x - 1, r.max.y - 1);
  BMessage message(B_INVALIDATE);
  message.AddRect("be:area", rect);
  sApp->window->PostMessage(&message, sApp->view);
}

Memdata *attachscreen(Rectangle *r, ulong *chan, int *depth, int *width,
                      int *softscreen) {
  *r = gscreen->clipr;
  *chan = gscreen->chan;
  *depth = gscreen->depth;
  *width = gscreen->width;
  *softscreen = 1;
  gscreen->data->ref++;
  return gscreen->data;
}

void getcolor(ulong i, ulong *r, ulong *g, ulong *b) {
  ulong v;
  v = cmap2rgb(i);
  *r = (v >> 16) & 0xFF;
  *g = (v >> 8) & 0xFF;
  *b = v & 0xFF;
}

void setcolor(ulong i, ulong r, ulong g, ulong b) {}

void setcursor(void) {
  static uint8 data[68];
  uint8 *p = data;
  lock(&cursor.lk);
  *p++ = 16;
  *p++ = 1;
  *p++ = -cursor.offset.x;
  *p++ = -cursor.offset.y;
  for (int i = 0; i < 32; i++) {
    p[i] = cursor.set[i];
    p[i + 32] = cursor.set[i] | cursor.clr[i];
  }
  sApp->SetCursor(data);
  unlock(&cursor.lk);
}

void mouseset(Point xy) {
  BAutolock locker(sApp->window);
  BMessage message(IS_SET_MOUSE_POSITION);
  BMessage reply;
  BPoint where(xy.x, xy.y);
  sApp->view->ConvertToScreen(&where);
  message.AddPoint("where", where);
  _control_input_server_(&message, &reply);
}

void screeninit(void) {
  BAutolock locker(sApp->window);
  BRect bounds = sApp->view->Bounds();
  Rectangle r = Rect(0, 0, bounds.right + 1, bounds.bottom + 1);
  memimageinit();
  screensize(r, XRGB32);
  gscreen->clipr = r;
  terminit();
}

static void cpuproc(void *arg) {
  rename_thread(find_thread(NULL), "cpu");
  cpubody();
}

void guimain(void) {
  Application app;
  kproc("cpu", cpuproc, nil);
  app.Run();
}

void screensize(Rectangle r, ulong chan) {
  static Memdata md = {
      .base = 0,
      .allocd = 0,
  };
  if (gscreen != nil)
    freememimage(gscreen);
  md.ref = 1;
  md.bdata = (uchar *)sApp->view->GetFrameBuffer();
  gscreen = allocmemimaged(r, chan, &md);
  gscreen->clipr = ZR;
}
