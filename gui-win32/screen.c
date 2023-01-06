#define _WIN32_WINNT 0x0500
#include	<windows.h>

#undef Rectangle
#define Rectangle _Rectangle

#include "u.h"
#include "lib.h"
#include "kern/dat.h"
#include "kern/fns.h"
#include "error.h"
#include "user.h"
#include <draw.h>
#include <memdraw.h>
#include "screen.h"
#include "keyboard.h"

Memimage	*gscreen;

static int depth;
static int dibtype;

static	HINSTANCE	inst;
static	HWND		window;
static	HPALETTE	palette;
static	LOGPALETTE	*logpal;
static  Lock		gdilock;
static 	BITMAPINFO	*bmi;
static	HCURSOR		hcursor;

static void	winproc(void *);
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static void	paletteinit(void);
static void	bmiinit(void);

static int readybit;
static Rendez	rend;

static int
isready(void*a)
{
	return readybit;
}

void
screeninit(void)
{
	int dx, dy;
	ulong chan;

	FreeConsole();
	memimageinit();

	if(depth == 0)
		depth = GetDeviceCaps(GetDC(NULL), BITSPIXEL);
	switch(depth){
	case 32:
		dibtype = DIB_RGB_COLORS;
		depth = 32;
		chan = XRGB32;
		break;
	case 24:
		dibtype = DIB_RGB_COLORS;
		depth = 24;
		chan = RGB24;
		break;
	case 16:
		dibtype = DIB_RGB_COLORS;
		depth = 16;
		chan = RGB15;	/* [sic] */
		break;
	case 8:
	default:
		dibtype = DIB_PAL_COLORS;
		depth = 8;
		depth = 8;
		chan = CMAP8;
		break;
	}
	dx = GetDeviceCaps(GetDC(NULL), HORZRES);
	dy = GetDeviceCaps(GetDC(NULL), VERTRES);
	screensize(Rect(0,0,dx,dy), chan);
	kproc("winscreen", winproc, 0);
	ksleep(&rend, isready, 0);
}

void
screensize(Rectangle r, ulong chan)
{
	Memimage *i;

	if((i = allocmemimage(r, chan)) == nil)
		return;
	if(gscreen != nil)
		freememimage(gscreen);
	gscreen = i;
	gscreen->clipr = ZR;
}

Memdata*
attachscreen(Rectangle *r, ulong *chan, int *depth, int *width, int *softscreen)
{
	*r = gscreen->clipr;
	*chan = gscreen->chan;
	*depth = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;

	gscreen->data->ref++;
	return gscreen->data;
}

void
flushmemscreen(Rectangle r)
{
	int dx, dy;
	HDC hdc;

	/*
	 * Sometimes we do get rectangles that are off the
	 * screen to the negative axes, for example, when
	 * dragging around a window border in a Move operation.
	 */
	if(rectclip(&r, gscreen->clipr) == 0)
		return;
	
	lock(&gdilock);

	hdc = GetDC(window);
	SelectPalette(hdc, palette, 0);
	RealizePalette(hdc);

	dx = r.max.x - r.min.x;
	dy = r.max.y - r.min.y;

	bmi->bmiHeader.biWidth = (gscreen->width*sizeof(ulong)*8)/gscreen->depth;
	bmi->bmiHeader.biHeight = -dy;	/* - => origin upper left */

	SetDIBitsToDevice(hdc,
		r.min.x, r.min.y,
		dx, dy,
		r.min.x, 0,
		0, dy,
		byteaddr(gscreen, Pt(0, r.min.y)), bmi,
		dibtype);

	ReleaseDC(window, hdc);

	GdiFlush();
 
	unlock(&gdilock);
}

static void
winproc(void *a)
{
	WNDCLASS wc;
	MSG msg;

	inst = GetModuleHandle(NULL);

	paletteinit();
	bmiinit();

	wc.style = 0;
	wc.lpfnWndProc = WindowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = inst;
	wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(101));
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = GetStockObject(WHITE_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"9pmgraphics";
	RegisterClass(&wc);

	window = CreateWindowEx(
		0,			/* extended style */
		L"9pmgraphics",		/* class */
		L"drawterm screen",		/* caption */
		WS_OVERLAPPEDWINDOW,    /* style */
		CW_USEDEFAULT,		/* init. x pos */
		CW_USEDEFAULT,		/* init. y pos */
		CW_USEDEFAULT,		/* init. x size */
		CW_USEDEFAULT,		/* init. y size */
		NULL,			/* parent window (actually owner window for overlapped)*/
		NULL,			/* menu handle */
		inst,			/* program handle */
		NULL			/* create parms */
		);

	if(window == nil)
		panic("can't make window\n");

	ShowWindow(window, SW_SHOWDEFAULT);
	UpdateWindow(window);

	terminit();

	readybit = 1;
	wakeup(&rend);

	while(GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
//	MessageBox(0, "winproc", "exits", MB_OK);
	ExitProcess(0);
}

int
col(int v, int n)
{
	int i, c;

	c = 0;
	for(i = 0; i < 8; i += n)
		c |= v << (16-(n+i));
	return c >> 8;
}


void
paletteinit(void)
{
	PALETTEENTRY *pal;
	int r, g, b, cr, cg, cb, v;
	int num, den;
	int i, j;

	logpal = mallocz(sizeof(LOGPALETTE) + 256*sizeof(PALETTEENTRY), 1);
	if(logpal == nil)
		panic("out of memory");
	logpal->palVersion = 0x300;
	logpal->palNumEntries = 256;
	pal = logpal->palPalEntry;

	for(r=0,i=0; r<4; r++) {
		for(v=0; v<4; v++,i+=16){
			for(g=0,j=v-r; g<4; g++) {
				for(b=0; b<4; b++,j++){
					den=r;
					if(g>den)
						den=g;
					if(b>den)
						den=b;
					/* divide check -- pick grey shades */
					if(den==0)
						cr=cg=cb=v*17;
					else{
						num=17*(4*den+v);
						cr=r*num/den;
						cg=g*num/den;
						cb=b*num/den;
					}
					pal[i+(j&15)].peRed = cr;
					pal[i+(j&15)].peGreen = cg;
					pal[i+(j&15)].peBlue = cb;
					pal[i+(j&15)].peFlags = 0;
				}
			}
		}
	}
	palette = CreatePalette(logpal);
}


void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
	PALETTEENTRY *pal;

	pal = logpal->palPalEntry;
	*r = pal[i].peRed;
	*g = pal[i].peGreen;
	*b = pal[i].peBlue;
}

void
bmiinit(void)
{
	ushort *p;
	int i;

	bmi = mallocz(sizeof(BITMAPINFOHEADER) + 256*sizeof(RGBQUAD), 1);
	if(bmi == 0)
		panic("out of memory");
	bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi->bmiHeader.biWidth = 0;
	bmi->bmiHeader.biHeight = 0;	/* - => origin upper left */
	bmi->bmiHeader.biPlanes = 1;
	bmi->bmiHeader.biBitCount = depth;
	bmi->bmiHeader.biCompression = BI_RGB;
	bmi->bmiHeader.biSizeImage = 0;
	bmi->bmiHeader.biXPelsPerMeter = 0;
	bmi->bmiHeader.biYPelsPerMeter = 0;
	bmi->bmiHeader.biClrUsed = 0;
	bmi->bmiHeader.biClrImportant = 0;	/* number of important colors: 0 means all */

	p = (ushort*)bmi->bmiColors;
	for(i = 0; i < 256; i++)
		p[i] = i;
}

void
togglefull(HWND hwnd)
{
	static int full;
	static LONG style, exstyle;
	static WINDOWPLACEMENT pl;
	MONITORINFO mi;
	
	full = !full;
	if(full){
		SendMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
		style = GetWindowLong(hwnd, GWL_STYLE);
		exstyle = GetWindowLong(hwnd, GWL_EXSTYLE);
		pl.length = sizeof(WINDOWPLACEMENT);
		GetWindowPlacement(hwnd, &pl);
		SetWindowLong(hwnd, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME));
		SetWindowLong(hwnd, GWL_EXSTYLE, exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
		mi.cbSize = sizeof(MONITORINFO);
		GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowPos(hwnd, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}else{
		SetWindowLong(hwnd, GWL_STYLE, style);
		SetWindowLong(hwnd, GWL_EXSTYLE, exstyle);
		SetWindowPlacement(hwnd, &pl);
	}
}

Rune vk2rune[256] = {
[VK_CANCEL] Kbreak,
[VK_CAPITAL] Kcaps,
[VK_CONTROL] Kctl,
[VK_DELETE] Kdel,
[VK_DOWN] Kdown,
[VK_END] Kend,
[VK_F1] KF|1,KF|2,KF|3,KF|4,KF|5,KF|6,KF|7,KF|8,KF|9,KF|10,KF|11,KF|12,
[VK_HOME] Khome,
[VK_INSERT] Kins,
[VK_LEFT] Kleft,
[VK_MENU] Kalt,
[VK_NEXT] Kpgdown,
[VK_NUMLOCK] Knum,
[VK_PRINT] Kprint,
[VK_PRIOR] Kpgup,
[VK_RIGHT] Kright,
[VK_RMENU] Kaltgr,
[VK_SCROLL] Kscroll,
[VK_SHIFT] Kshift,
[VK_UP] Kup,
};
		

LRESULT CALLBACK
WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	static Rune scdown[256];
	PAINTSTRUCT paint;
	HDC hdc;
	LONG x, y, b;
	int i;
	RECT winr;
	Rectangle r;
	Rune k;

	b = 0;

	switch(msg) {
	case WM_CREATE:
		if(GetClientRect(hwnd, &winr) == 0)
			break;
		gscreen->clipr = Rect(0, 0, winr.right - winr.left, winr.bottom - winr.top);
		rectclip(&gscreen->clipr, gscreen->r);
		break;

	case WM_SETCURSOR:
		/* User set */
		if(hcursor != NULL) {
			SetCursor(hcursor);
			return 1;
		}
		return DefWindowProc(hwnd, msg, wparam, lparam);
	case WM_MOUSEWHEEL:
		if ((int)(wparam & 0xFFFF0000)>0)
			b |=8;
		else
			b |=16;
		mousetrack(0, 0, b, ticks());
		break;
	case WM_MOUSEMOVE:
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		x = LOWORD(lparam);
		y = HIWORD(lparam);
		if(wparam & MK_LBUTTON)
			b |= 1;
		if(wparam & MK_MBUTTON)
			b |= 2;
		if(wparam & MK_RBUTTON)
			b |= 4;
		absmousetrack(x, y, b, ticks());
		break;

	case WM_CHAR:
		k = wparam;
		if(k == '\n')
			k = '\r';
		else if(k == '\r')
			k = '\n';
		if(0){
	case WM_SYSKEYDOWN:
	case WM_KEYDOWN:
			k = vk2rune[wparam&0xFF];
		}
		if(k == 0)
			break;
		i = (lparam>>16)&0xFF;
		scdown[i] = k;
		kbdkey(k, 1);
		break;
	case WM_SYSKEYUP:
	case WM_KEYUP:
		if(wparam == VK_PAUSE)
			togglefull(hwnd);
		i = (lparam>>16)&0xFF;
		k = scdown[i];
		if(k != 0){
			scdown[i] = 0;
			kbdkey(k, 0);
		}
		break;

	case WM_CLOSE:
		DestroyWindow(hwnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_PALETTECHANGED:
		if((HWND)wparam == hwnd)
			break;
	/* fall through */
	case WM_QUERYNEWPALETTE:
		hdc = GetDC(hwnd);
		SelectPalette(hdc, palette, 0);
		if(RealizePalette(hdc) != 0)
			InvalidateRect(hwnd, nil, 0);
		ReleaseDC(hwnd, hdc);
		break;

	case WM_PAINT:
		hdc = BeginPaint(hwnd, &paint);
		r.min.x = paint.rcPaint.left;
		r.min.y = paint.rcPaint.top;
		r.max.x = paint.rcPaint.right;
		r.max.y = paint.rcPaint.bottom;
		flushmemscreen(r);
		EndPaint(hwnd, &paint);
		break;

	case WM_SIZE:
		if(GetClientRect(hwnd, &winr) == 0)
			break;
		screenresize(Rect(0, 0, winr.right - winr.left, winr.bottom - winr.top));
		break;

	case WM_COMMAND:
	case WM_SETFOCUS:
	case WM_DEVMODECHANGE:
	case WM_WININICHANGE:
	case WM_INITMENU:
	default:
		return DefWindowProc(hwnd, msg, wparam, lparam);
	}
	return 0;
}

void
mouseset(Point xy)
{
	POINT pt;

	pt.x = xy.x;
	pt.y = xy.y;
	MapWindowPoints(window, 0, &pt, 1);
	SetCursorPos(pt.x, pt.y);
}

void
setcursor(void)
{
	HCURSOR nh;
	int x, y, h, w;
	uchar *sp, *cp;
	uchar *and, *xor;

	h = GetSystemMetrics(SM_CYCURSOR);
	w = (GetSystemMetrics(SM_CXCURSOR)+7)/8;

	and = mallocz(h*w, 1);
	memset(and, 0xff, h*w);
	xor = mallocz(h*w, 1);
	
	lock(&cursor.lk);
	for(y=0,sp=cursor.set,cp=cursor.clr; y<16; y++) {
		for(x=0; x<2; x++) {
			and[y*w+x] = ~(*sp|*cp);
			xor[y*w+x] = ~*sp & *cp;
			cp++;
			sp++;
		}
	}
	nh = CreateCursor(inst, -cursor.offset.x, -cursor.offset.y,
			GetSystemMetrics(SM_CXCURSOR), h,
			and, xor);
	if(nh != NULL) {
		SetCursor(nh);
		if(hcursor != NULL)
			DestroyCursor(hcursor);
		hcursor = nh;
	}
	unlock(&cursor.lk);

	free(and);
	free(xor);

	PostMessage(window, WM_SETCURSOR, (WPARAM)window, 0);
}

void
cursorarrow(void)
{
	if(hcursor != 0) {
		DestroyCursor(hcursor);
		hcursor = 0;
	}
	SetCursor(LoadCursor(0, IDC_ARROW));
	PostMessage(window, WM_SETCURSOR, (WPARAM)window, 0);
}


void
setcolor(ulong index, ulong red, ulong green, ulong blue)
{
}


char*
clipreadunicode(HANDLE h)
{
	wchar_t *p;
	int n;
	char *q;

	p = GlobalLock(h);
	n = WideCharToMultiByte(CP_UTF8, 0, p, -1, 0, 0, 0, 0);
	q = malloc(n+1);
	WideCharToMultiByte(CP_UTF8, 0, p, -1, q, n, 0, 0);
	GlobalUnlock(h);

	return q;
}

char*
clipreadutf(HANDLE h)
{
	char *p;

	p = GlobalLock(h);
	p = strdup(p);
	GlobalUnlock(h);
	
	return p;
}

char*
clipread(void)
{
	HANDLE h;
	char *p, *q, *r;

	if(!OpenClipboard(window)){
		oserror();
		return strdup("");
	}

	if((h = GetClipboardData(CF_UNICODETEXT)))
		p = clipreadunicode(h);
	else if((h = GetClipboardData(CF_TEXT)))
		p = clipreadutf(h);
	else {
		oserror();
		return strdup("");
	}

	for(q = r = p; *q != 0; q++)
		if(*q != '\r')
			*r++ = *q;
	*r = 0;
	
	CloseClipboard();
	return p;
}

static char *
addcr(char *buf, int *lp)
{
	int nlen;
	char *r, *p, *q;

	nlen = 0;
	for(p = buf; *p != 0; p++){
		if(*p == '\n')
			nlen++;
		nlen++;
	}
	*lp = nlen;
	r = malloc(nlen + 1);
	if(r == nil)
		panic("malloc: %r");
	q = r;
	for(p = buf; *p != 0; p++){
		if(*p == '\n')
			*q++ = '\r';
		*q++ = *p;
	}
	*q = 0;
	return r;
}

int
clipwrite(char *buf)
{
	HANDLE h;
	char *p;
	wchar_t *rp;
	char *crbuf;
	int n;

	if(!OpenClipboard(window)) {
		oserror();
		return -1;
	}

	if(!EmptyClipboard()) {
		oserror();
		CloseClipboard();
		return -1;
	}

	crbuf = addcr(buf, &n);

	h = GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE, (n+1)*sizeof(rp[0]));
	if(h == NULL)
		panic("out of memory");
	rp = GlobalLock(h);
	MultiByteToWideChar(CP_UTF8, 0, crbuf, -1, rp, n+1);
	GlobalUnlock(h);

	SetClipboardData(CF_UNICODETEXT, h);

	h = GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE, n+1);
	if(h == NULL)
		panic("out of memory");
	p = GlobalLock(h);
	memcpy(p, crbuf, n);
	p[n] = 0;
	GlobalUnlock(h);
	
	SetClipboardData(CF_TEXT, h);

	CloseClipboard();
	free(crbuf);
	return n;
}

void
guimain(void)
{
	cpubody();
}
