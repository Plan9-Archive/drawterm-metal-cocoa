#define Rect RectC
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#undef Rect

#undef nil
#define Point Point9
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

#ifndef DEBUG
#define DEBUG 0
#endif
#define LOG(fmt, ...) if(DEBUG)NSLog((@"%s:%d %s " fmt), __FILE__, __LINE__, __PRETTY_FUNCTION__, ##__VA_ARGS__)

Memimage *gscreen;

@interface DrawLayer : CAMetalLayer
@property id<MTLTexture> texture;
@end

@interface DrawtermView : NSView<NSTextInputClient>
- (void)reshape;
- (void)clearMods;
- (void)clearInput;
- (void)mouseevent:(NSEvent *)e;
- (void)resetLastInputRect;
- (void)enlargeLastInputRect:(NSRect)r;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

static AppDelegate *myApp;
static DrawtermView *myview;
static NSCursor *currentCursor;

static ulong pal[256];

static int readybit;
static Rendez rend;

static int
isready(void*a)
{
	return readybit;
}

void
guimain(void)
{
	LOG();
	@autoreleasepool{
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		myApp = [AppDelegate new];
		[NSApp setDelegate:myApp];
		[NSApp run];
	}
}

void
screeninit(void)
{
	memimageinit();
	NSSize s = [myview convertSizeToBacking:myview.frame.size];
	screensize(Rect(0, 0, s.width, s.height), ARGB32);
	gscreen->clipr = Rect(0, 0, s.width, s.height);
	LOG(@"%g %g", s.width, s.height);
	terminit();
	readybit = 1;
	wakeup(&rend);
}

void
screensize(Rectangle r, ulong chan)
{
	Memimage *i;

	if((i = allocmemimage(r, chan)) == nil)
		return;
	if(gscreen != nil)
		freememimage(gscreen);
@autoreleasepool{
	DrawLayer *layer = (DrawLayer *)myview.layer;
	MTLTextureDescriptor *textureDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		width:Dx(r)
		height:Dy(r)
		mipmapped:NO];
	textureDesc.allowGPUOptimizedContents = YES;
	textureDesc.usage = MTLTextureUsageShaderRead;
	textureDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
	layer.texture = [layer.device newTextureWithDescriptor:textureDesc];

	CGFloat scale = myview.window.backingScaleFactor;
	[layer setDrawableSize:NSMakeSize(Dx(r), Dy(r))];
	[layer setContentsScale:scale];
}
	gscreen = i;
	gscreen->clipr = ZR;
}

Memdata*
attachscreen(Rectangle *r, ulong *chan, int *depth, int *width, int *softscreen)
{
	LOG();
	*r = gscreen->clipr;
	*chan = gscreen->chan;
	*depth = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;

	gscreen->data->ref++;
	return gscreen->data;
}

char *
clipread(void)
{
	@autoreleasepool{
		NSPasteboard *pb = [NSPasteboard generalPasteboard];
		NSString *s = [pb stringForType:NSPasteboardTypeString];
		if(s)
			return strdup([s UTF8String]);
	}
	return nil;
}

int
clipwrite(char *buf)
{
	@autoreleasepool{
		NSString *s = [[NSString alloc] initWithUTF8String:buf];
		NSPasteboard *pb = [NSPasteboard generalPasteboard];
		[pb clearContents];
		[pb writeObjects:@[s]];
	}
	return strlen(buf);
}

void
flushmemscreen(Rectangle r)
{
	LOG(@"<- %d %d %d %d", r.min.x, r.min.y, Dx(r), Dy(r));
	if(rectclip(&r, gscreen->clipr) == 0)
		return;
	LOG(@"-> %d %d %d %d", r.min.x, r.min.y, Dx(r), Dy(r));
	@autoreleasepool{
		[((DrawLayer *)myview.layer).texture
			replaceRegion:MTLRegionMake2D(r.min.x, r.min.y, Dx(r), Dy(r))
			mipmapLevel:0
			withBytes:byteaddr(gscreen, Pt(r.min.x, r.min.y))
			bytesPerRow:gscreen->width * 4];
		NSRect sr = [[myview window] convertRectFromBacking:NSMakeRect(r.min.x, r.min.y, Dx(r), Dy(r))];
		dispatch_async(dispatch_get_main_queue(), ^(void){@autoreleasepool{
			LOG(@"setNeedsDisplayInRect %g %g %g %g", sr.origin.x, sr.origin.y, sr.size.width, sr.size.height);
			[myview setNeedsDisplayInRect:sr];
			[myview enlargeLastInputRect:sr];
		}});
		// ReplaceRegion is somehow asynchronous since 10.14.5.  We wait sometime to request a update again.
		dispatch_time_t time = dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_MSEC);
		dispatch_after(time, dispatch_get_main_queue(), ^(void){@autoreleasepool{
			LOG(@"setNeedsDisplayInRect %g %g %g %g again", sr.origin.x, sr.origin.y, sr.size.width, sr.size.height);
			[myview setNeedsDisplayInRect:sr];
		}});
		time = dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC);
		dispatch_after(time, dispatch_get_main_queue(), ^(void){@autoreleasepool{
			LOG(@"setNeedsDisplayInRect %g %g %g %g again", sr.origin.x, sr.origin.y, sr.size.width, sr.size.height);
			[myview setNeedsDisplayInRect:sr];
		}});
	}
}

void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
	ulong v;

	v = pal[i];
	*r = (v>>16)&0xFF;
	*g = (v>>8)&0xFF;
	*b = v&0xFF;
}

void
setcolor(ulong i, ulong r, ulong g, ulong b)
{
	pal[i] = ((r&0xFF)<<16) & ((g&0xFF)<<8) & (b&0xFF);
}

void
setcursor(void)
{
	static unsigned char data[64], data2[256];
	unsigned char *planes[2] = {&data[0], &data[32]};
	unsigned char *planes2[2] = {&data2[0], &data2[128]};
	unsigned int i, x, y, a;
	unsigned char pu, pb, pl, pr, pc;  // upper, bottom, left, right, center
	unsigned char pul, pur, pbl, pbr;
	unsigned char ful, fur, fbl, fbr;

	lock(&cursor.lk);
	for(i = 0; i < 32; i++){
		data[i] = ~cursor.set[i] & cursor.clr[i];
		data[i+32] = cursor.set[i] | cursor.clr[i];
	}
	for(a=0; a<2; a++){
		for(y=0; y<16; y++){
			for(x=0; x<2; x++){
				pc = planes[a][x+2*y];
				pu = y==0 ? pc : planes[a][x+2*(y-1)];
				pb = y==15 ? pc : planes[a][x+2*(y+1)];
				pl = (pc>>1) | (x==0 ? pc&0x80 : (planes[a][x-1+2*y]&1)<<7);
				pr = (pc<<1) | (x==1 ? pc&1 : (planes[a][x+1+2*y]&0x80)>>7);
				ful = ~(pl^pu) & (pl^pb) & (pu^pr);
				pul = (ful & pu) | (~ful & pc);
				fur = ~(pu^pr) & (pu^pl) & (pr^pb);
				pur = (fur & pr) | (~fur & pc);
				fbl = ~(pb^pl) & (pb^pr) & (pl^pu);
				pbl = (fbl & pl) | (~fbl & pc);
				fbr = ~(pr^pb) & (pr^pu) & (pb^pl);
				pbr = (fbr & pb) | (~fbr & pc);
				planes2[a][2*x+4*2*y] = (pul&0x80) | ((pul&0x40)>>1)  | ((pul&0x20)>>2) | ((pul&0x10)>>3)
					| ((pur&0x80)>>1) | ((pur&0x40)>>2)  | ((pur&0x20)>>3) | ((pur&0x10)>>4);
				planes2[a][2*x+1+4*2*y] = ((pul&0x8)<<4) | ((pul&0x4)<<3)  | ((pul&0x2)<<2) | ((pul&0x1)<<1)
					| ((pur&0x8)<<3) | ((pur&0x4)<<2)  | ((pur&0x2)<<1) | (pur&0x1);
				planes2[a][2*x+4*(2*y+1)] =  (pbl&0x80) | ((pbl&0x40)>>1)  | ((pbl&0x20)>>2) | ((pbl&0x10)>>3)
					| ((pbr&0x80)>>1) | ((pbr&0x40)>>2)  | ((pbr&0x20)>>3) | ((pbr&0x10)>>4);
				planes2[a][2*x+1+4*(2*y+1)] = ((pbl&0x8)<<4) | ((pbl&0x4)<<3)  | ((pbl&0x2)<<2) | ((pbl&0x1)<<1)
					| ((pbr&0x8)<<3) | ((pbr&0x4)<<2)  | ((pbr&0x2)<<1) | (pbr&0x1);
			}
		}
	}
	NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
		initWithBitmapDataPlanes:planes
		pixelsWide:16
		pixelsHigh:16
		bitsPerSample:1
		samplesPerPixel:2
		hasAlpha:YES
		isPlanar:YES
		colorSpaceName:NSDeviceWhiteColorSpace
		bitmapFormat:0
		bytesPerRow:2
		bitsPerPixel:0];
	NSBitmapImageRep *rep2 = [[NSBitmapImageRep alloc]
		initWithBitmapDataPlanes:planes2
		pixelsWide:32
		pixelsHigh:32
		bitsPerSample:1
		samplesPerPixel:2
		hasAlpha:YES
		isPlanar:YES
		colorSpaceName:NSDeviceWhiteColorSpace
		bitmapFormat:0
		bytesPerRow:4
		bitsPerPixel:0];
	NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
	[img addRepresentation:rep2];
	[img addRepresentation:rep];
	currentCursor = [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(-cursor.offset.x, -cursor.offset.y)];
	unlock(&cursor.lk);

	dispatch_async(dispatch_get_main_queue(), ^(void){
		[[myview window] invalidateCursorRectsForView:myview];
	});
}

void
mouseset(Point p)
{
	dispatch_async(dispatch_get_main_queue(), ^(void){@autoreleasepool{
		NSPoint s;

		if([[myview window] isKeyWindow]){
			s = NSMakePoint(p.x, p.y);
			LOG(@"-> pixel  %g %g", s.x, s.y);
			s = [[myview window] convertPointFromBacking:s];
			LOG(@"-> point  %g %g", s.x, s.y);
			s = [myview convertPoint:s toView:nil];
			LOG(@"-> window %g %g", s.x, s.y);
			s = [[myview window] convertPointToScreen: s];
			LOG(@"(%g, %g) <- toScreen", s.x, s.y);
			s.y = NSScreen.screens[0].frame.size.height - s.y;
			LOG(@"(%g, %g) <- setmouse", s.x, s.y);
			CGWarpMouseCursorPosition(s);
			CGAssociateMouseAndMouseCursorPosition(true);
		}
	}});
}

@implementation AppDelegate
{
	NSWindow *_window;
}

static void
mainproc(void *aux)
{
	cpubody();
}

- (void) applicationDidFinishLaunching:(NSNotification *)aNotification
{
	LOG(@"BEGIN");

	NSMenu *sm = [NSMenu new];
	[sm addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
	[sm addItemWithTitle:@"Hide" action:@selector(hide:) keyEquivalent:@"h"];
	[sm addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
	NSMenu *m = [NSMenu new];
	[m addItemWithTitle:@"DEVDRAW" action:NULL keyEquivalent:@""];
	[m setSubmenu:sm forItem:[m itemWithTitle:@"DEVDRAW"]];
	[NSApp setMainMenu:m];

	const NSWindowStyleMask Winstyle = NSWindowStyleMaskTitled
		| NSWindowStyleMaskClosable
		| NSWindowStyleMaskMiniaturizable
		| NSWindowStyleMaskResizable;

	NSRect r = [[NSScreen mainScreen] visibleFrame];

	r.size.width = r.size.width*3/4;
	r.size.height = r.size.height*3/4;
	r = [NSWindow contentRectForFrameRect:r styleMask:Winstyle];

	_window = [[NSWindow alloc] initWithContentRect:r styleMask:Winstyle
		backing:NSBackingStoreBuffered defer:NO];
	[_window setTitle:@"drawterm"];
	[_window center];
	[_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
	[_window setContentMinSize:NSMakeSize(64,64)];
	[_window setOpaque:YES];
	[_window setRestorable:NO];
	[_window setAcceptsMouseMovedEvents:YES];
	[_window setDelegate:self];

	myview = [DrawtermView new];
	[_window setContentView:myview];

	[NSEvent setMouseCoalescingEnabled:NO];
	setcursor();

	[_window makeKeyAndOrderFront:self];
	[NSApp activateIgnoringOtherApps:YES];

	LOG(@"launch mainproc");
	kproc("mainproc", mainproc, 0);
	ksleep(&rend, isready, 0);
}

- (NSApplicationPresentationOptions) window:(NSWindow *)window
		willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposedOptions
{
	NSApplicationPresentationOptions o;
	o = proposedOptions;
	o &= ~(NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar);
	o |= NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
	return o;
}

- (BOOL) applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)theApplication
{
	return YES;
}

- (void) windowDidBecomeKey:(id)arg
{
	NSPoint p;
	p = [_window convertPointToBacking:[_window mouseLocationOutsideOfEventStream]];
	absmousetrack(p.x, [myview convertSizeToBacking:myview.frame.size].height - p.y, 0, ticks());
}

- (void) windowDidResignKey:(id)arg
{
	[myview clearMods];
}

@end

@implementation DrawtermView
{
	NSMutableString *_tmpText;
	NSRange _markedRange;
	NSRange _selectedRange;
	NSRect _lastInputRect;	// The view is flipped, this is not.
	BOOL _tapping;
	NSUInteger _tapFingers;
	NSUInteger _tapTime;
	BOOL _breakcompose;
	NSEventModifierFlags _mods;
}

- (id) initWithFrame:(NSRect)fr
{
	LOG(@"BEGIN");
	self = [super initWithFrame:fr];
	[self setWantsLayer:YES];
	[self setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawOnSetNeedsDisplay];
	[self setAllowedTouchTypes:NSTouchTypeMaskDirect|NSTouchTypeMaskIndirect];
	_tmpText = [[NSMutableString alloc] initWithCapacity:2];
	_markedRange = NSMakeRange(NSNotFound, 0);
	_selectedRange = NSMakeRange(0, 0);
	_breakcompose = NO;
	_mods = 0;
	LOG(@"END");
	return self;
}

- (CALayer *) makeBackingLayer
{
	return [DrawLayer layer];
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (BOOL)isOpaque
{
	return YES;
}

- (BOOL)isFlipped
{
	return YES;
}

static uint
evkey(uint v)
{
	switch(v){
	case '\r': return '\n';
	case 127: return '\b';
	case NSUpArrowFunctionKey: return Kup;
	case NSDownArrowFunctionKey: return Kdown;
	case NSLeftArrowFunctionKey: return Kleft;
	case NSRightArrowFunctionKey: return Kright;
	case NSF1FunctionKey: return KF|1;
	case NSF2FunctionKey: return KF|2;
	case NSF3FunctionKey: return KF|3;
	case NSF4FunctionKey: return KF|4;
	case NSF5FunctionKey: return KF|5;
	case NSF6FunctionKey: return KF|6;
	case NSF7FunctionKey: return KF|7;
	case NSF8FunctionKey: return KF|8;
	case NSF9FunctionKey: return KF|9;
	case NSF10FunctionKey: return KF|10;
	case NSF11FunctionKey: return KF|11;
	case NSF12FunctionKey: return KF|12;
	case NSInsertFunctionKey: return Kins;
	case NSDeleteFunctionKey: return Kdel;
	case NSHomeFunctionKey: return Khome;
	case NSEndFunctionKey: return Kend;
	case NSPageUpFunctionKey: return Kpgup;
	case NSPageDownFunctionKey: return Kpgdown;
	case NSScrollLockFunctionKey: return Kscroll;
	case NSBeginFunctionKey:
	case NSF13FunctionKey:
	case NSF14FunctionKey:
	case NSF15FunctionKey:
	case NSF16FunctionKey:
	case NSF17FunctionKey:
	case NSF18FunctionKey:
	case NSF19FunctionKey:
	case NSF20FunctionKey:
	case NSF21FunctionKey:
	case NSF22FunctionKey:
	case NSF23FunctionKey:
	case NSF24FunctionKey:
	case NSF25FunctionKey:
	case NSF26FunctionKey:
	case NSF27FunctionKey:
	case NSF28FunctionKey:
	case NSF29FunctionKey:
	case NSF30FunctionKey:
	case NSF31FunctionKey:
	case NSF32FunctionKey:
	case NSF33FunctionKey:
	case NSF34FunctionKey:
	case NSF35FunctionKey:
	case NSPrintScreenFunctionKey:
	case NSPauseFunctionKey:
	case NSSysReqFunctionKey:
	case NSBreakFunctionKey:
	case NSResetFunctionKey:
	case NSStopFunctionKey:
	case NSMenuFunctionKey:
	case NSUserFunctionKey:
	case NSSystemFunctionKey:
	case NSPrintFunctionKey:
	case NSClearLineFunctionKey:
	case NSClearDisplayFunctionKey:
	case NSInsertLineFunctionKey:
	case NSDeleteLineFunctionKey:
	case NSInsertCharFunctionKey:
	case NSDeleteCharFunctionKey:
	case NSPrevFunctionKey:
	case NSNextFunctionKey:
	case NSSelectFunctionKey:
	case NSExecuteFunctionKey:
	case NSUndoFunctionKey:
	case NSRedoFunctionKey:
	case NSFindFunctionKey:
	case NSHelpFunctionKey:
	case NSModeSwitchFunctionKey: return 0;
	default: return v;
	}
}

- (void)keyDown:(NSEvent*)event {
	[self interpretKeyEvents:[NSArray arrayWithObject:event]];
	[self resetLastInputRect];
}

- (void)flagsChanged:(NSEvent*)event {
	NSEventModifierFlags x;
	NSUInteger u;

	x = [event modifierFlags];
	u = [NSEvent pressedMouseButtons];
	u = (u&~6) | (u&4)>>1 | (u&2)<<1;
	if((x & ~_mods & NSEventModifierFlagShift) != 0)
		kbdkey(Kshift, 1);
	if((x & ~_mods & NSEventModifierFlagControl) != 0){
		if(u){
			u |= 1;
			[self sendmouse:u];
			return;
		}else
			kbdkey(Kctl, 1);
	}
	if((x & ~_mods & NSEventModifierFlagOption) != 0){
		if(u){
			u |= 2;
			[self sendmouse:u];
			return;
		}else
			kbdkey(Kalt, 1);
	}
	if((x & NSEventModifierFlagCommand) != 0)
		if(u){
			u |= 4;
			[self sendmouse:u];
		}
	if((x & ~_mods & NSEventModifierFlagCapsLock) != 0)
		kbdkey(Kcaps, 1);
	if((~x & _mods & NSEventModifierFlagShift) != 0)
		kbdkey(Kshift, 0);
	if((~x & _mods & NSEventModifierFlagControl) != 0)
		kbdkey(Kctl, 0);
	if((~x & _mods & NSEventModifierFlagOption) != 0){
		kbdkey(Kalt, 0);
		if(_breakcompose){
			kbdkey(Kalt, 1);
			kbdkey(Kalt, 0);
			_breakcompose = NO;
		}
	}
	if((~x & _mods & NSEventModifierFlagCapsLock) != 0)
		kbdkey(Kcaps, 0);
	_mods = x;
}

- (void) clearMods {
	if((_mods & NSEventModifierFlagShift) != 0){
		kbdkey(Kshift, 0);
		_mods ^= NSEventModifierFlagShift;
	}
	if((_mods & NSEventModifierFlagControl) != 0){
		kbdkey(Kctl, 0);
		_mods ^= NSEventModifierFlagControl;
	}
	if((_mods & NSEventModifierFlagOption) != 0){
		kbdkey(Kalt, 0);
		_mods ^= NSEventModifierFlagOption;
	}
}

- (void) mouseevent:(NSEvent*)event
{
	NSPoint p;
	Point q;
	NSUInteger u;
	NSEventModifierFlags m;

	p = [self.window convertPointToBacking:[self.window mouseLocationOutsideOfEventStream]];
	u = [NSEvent pressedMouseButtons];
	q.x = p.x;
	q.y = p.y;
	if(!ptinrect(q, gscreen->clipr)) return;
	u = (u&~6) | (u&4)>>1 | (u&2)<<1;
	if(u == 1){
		m = [event modifierFlags];
		if(m & NSEventModifierFlagOption){
			_breakcompose = 1;
			u = 2;
		}else if(m & NSEventModifierFlagCommand)
			u = 4;
	}
	absmousetrack(p.x, [self convertSizeToBacking:self.frame.size].height - p.y, u, ticks());
	if(u && _lastInputRect.size.width && _lastInputRect.size.height)
		[self resetLastInputRect];
}

- (void) sendmouse:(NSUInteger)u
{
	mousetrack(0, 0, u, ticks());
	if(u && _lastInputRect.size.width && _lastInputRect.size.height)
		[self resetLastInputRect];
}

- (void) mouseDown:(NSEvent*)event { [self mouseevent:event]; }
- (void) mouseDragged:(NSEvent*)event { [self mouseevent:event]; }
- (void) mouseUp:(NSEvent*)event { [self mouseevent:event]; }
- (void) mouseMoved:(NSEvent*)event { [self mouseevent:event]; }
- (void) rightMouseDown:(NSEvent*)event { [self mouseevent:event]; }
- (void) rightMouseDragged:(NSEvent*)event { [self mouseevent:event]; }
- (void) rightMouseUp:(NSEvent*)event { [self mouseevent:event]; }
- (void) otherMouseDown:(NSEvent*)event { [self mouseevent:event]; }
- (void) otherMouseDragged:(NSEvent*)event { [self mouseevent:event]; }
- (void) otherMouseUp:(NSEvent*)event { [self mouseevent:event]; }

- (void) scrollWheel:(NSEvent*)event{
	NSInteger s = [event scrollingDeltaY];
	if(s > 0)
		[self sendmouse:8];
	else if(s < 0)
		[self sendmouse:16];
}

- (void)magnifyWithEvent:(NSEvent*)e{
	if(fabs([e magnification]) > 0.02)
		[[self window] toggleFullScreen:nil];
}

- (void)touchesBeganWithEvent:(NSEvent*)e
{
	_tapping = YES;
	_tapFingers = [e touchesMatchingPhase:NSTouchPhaseTouching inView:nil].count;
	_tapTime = ticks();
}

- (void)touchesMovedWithEvent:(NSEvent*)e
{
	_tapping = NO;
}

- (void)touchesEndedWithEvent:(NSEvent*)e
{
	if(_tapping
		&& [e touchesMatchingPhase:NSTouchPhaseTouching inView:nil].count == 0
		&& ticks() - _tapTime < 250){
		switch(_tapFingers){
		case 3:
			[self sendmouse:2];
			[self sendmouse:0];
			break;
		case 4:
			[self sendmouse:2];
			[self sendmouse:1];
			[self sendmouse:0];
			break;
		}
		_tapping = NO;
	}
}

- (void)touchesCancelledWithEvent:(NSEvent*)e
{
	_tapping = NO;
}

- (BOOL) acceptsFirstResponder
{
	return TRUE;
}

- (void) resetCursorRects
{
	[super resetCursorRects];
	lock(&cursor.lk);
	[self addCursorRect:self.bounds cursor:currentCursor];
	unlock(&cursor.lk);
}

- (void) reshape
{
	NSSize s = [self convertSizeToBacking:self.frame.size];
	LOG(@"%g %g", s.width, s.height);
	if(gscreen != nil){
		screenresize(Rect(0, 0, s.width, s.height));
	}
}

- (void)windowDidResize:(NSNotification *)notification
{
	if(![self inLiveResize])
		[self reshape];
}

- (void)viewDidEndLiveResize
{
	LOG();
	[super viewDidEndLiveResize];
	[self reshape];
}

- (void)viewDidChangeBackingProperties
{
	LOG();
	[super viewDidChangeBackingProperties];
	[self reshape];
}

static void
keystroke(Rune r)
{
	kbdkey(r, 1);
	kbdkey(r, 0);
}

// conforms to protocol NSTextInputClient
- (BOOL)hasMarkedText
{
	return _markedRange.location != NSNotFound;
}
- (NSRange)markedRange
{
	return _markedRange;
}
- (NSRange)selectedRange
{
	return _selectedRange;
}
- (void)setMarkedText:(id)string
	selectedRange:(NSRange)sRange
	replacementRange:(NSRange)rRange
{
	NSString *str;

	[self clearInput];

	if([string isKindOfClass:[NSAttributedString class]])
		str = [string string];
	else
		str = string;

	if(rRange.location == NSNotFound){
		if(_markedRange.location != NSNotFound){
			rRange = _markedRange;
		}else{
			rRange = _selectedRange;
		}
	}

	if(str.length == 0){
		[_tmpText deleteCharactersInRange:rRange];
		[self unmarkText];
	}else{
		_markedRange = NSMakeRange(rRange.location, str.length);
		[_tmpText replaceCharactersInRange:rRange withString:str];
	}
	_selectedRange.location = rRange.location + sRange.location;
	_selectedRange.length = sRange.length;

	if(_tmpText.length){
		for(uint i = 0; i <= _tmpText.length; ++i){
			if(i == _markedRange.location)
				keystroke('[');
			if(_selectedRange.length){
				if(i == _selectedRange.location)
					keystroke('{');
				if(i == NSMaxRange(_selectedRange))
					keystroke('}');
				}
			if(i == NSMaxRange(_markedRange))
				keystroke(']');
			if(i < _tmpText.length)
				keystroke([_tmpText characterAtIndex:i]);
		}
		uint l = 1 + _tmpText.length - NSMaxRange(_selectedRange)
			+ (_selectedRange.length > 0);
		for(uint i = 0; i < l; ++i)
			keystroke(Kleft);
	}
}
- (void)unmarkText
{
	[_tmpText deleteCharactersInRange:NSMakeRange(0, [_tmpText length])];
	_markedRange = NSMakeRange(NSNotFound, 0);
	_selectedRange = NSMakeRange(0, 0);
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText
{
	return @[];
}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r
	actualRange:(NSRangePointer)actualRange
{
	NSRange sr;
	NSAttributedString *s;

	sr = NSMakeRange(0, [_tmpText length]);
	sr = NSIntersectionRange(sr, r);
	if(actualRange)
		*actualRange = sr;
	if(sr.length)
		s = [[NSAttributedString alloc]
			initWithString:[_tmpText substringWithRange:sr]];
	return s;
}
- (void)insertText:(id)s
	replacementRange:(NSRange)r
{
	NSUInteger i;
	NSUInteger len;

	[self clearInput];

	len = [s length];
	for(i = 0; i < len; ++i)
		keystroke([s characterAtIndex:i]);
	[_tmpText deleteCharactersInRange:NSMakeRange(0, _tmpText.length)];
	_markedRange = NSMakeRange(NSNotFound, 0);
	_selectedRange = NSMakeRange(0, 0);
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
	return 0;
}
- (NSRect)firstRectForCharacterRange:(NSRange)r
	actualRange:(NSRangePointer)actualRange
{
	if(actualRange)
		*actualRange = r;
	return [[self window] convertRectToScreen:_lastInputRect];
}
- (void)doCommandBySelector:(SEL)s
{
	NSEvent *e;
	uint c, k;

	e = [NSApp currentEvent];
	c = [[e charactersIgnoringModifiers] characterAtIndex:0];
	k = evkey(c);
	if(k>0)
		keystroke(k);
}

// Helper for managing input rect approximately
- (void)resetLastInputRect
{
	_lastInputRect.origin.x = 0.0;
	_lastInputRect.origin.y = 0.0;
	_lastInputRect.size.width = 0.0;
	_lastInputRect.size.height = 0.0;
}

- (void)enlargeLastInputRect:(NSRect)r
{
	r.origin.y = [self bounds].size.height - r.origin.y - r.size.height;
	_lastInputRect = NSUnionRect(_lastInputRect, r);
}

- (void)clearInput
{
	if(_tmpText.length){
		uint l = 1 + _tmpText.length - NSMaxRange(_selectedRange)
			+ (_selectedRange.length > 0);
		for(uint i = 0; i < l; ++i)
			keystroke(Kright);
		l = _tmpText.length+2+2*(_selectedRange.length > 0);
		for(uint i = 0; i < l; ++i)
			keystroke(Kbs);
	}
}
@end

@implementation DrawLayer
{
	id<MTLCommandQueue> _commandQueue;
}

- (id) init {
	LOG();
	self = [super init];
	self.device = MTLCreateSystemDefaultDevice();
	self.pixelFormat = MTLPixelFormatBGRA8Unorm;
	self.framebufferOnly = YES;
	self.opaque = YES;

	// We use a default transparent layer on top of the CAMetalLayer.
	// This seems to make fullscreen applications behave.
	{
		CALayer *stub = [CALayer layer];
		stub.frame = CGRectMake(0, 0, 1, 1);
		[stub setNeedsDisplay];
		[self addSublayer:stub];
	}

	_commandQueue = [self.device newCommandQueue];

	return self;
}

- (void) display
{
	id<MTLCommandBuffer> cbuf;
	id<MTLBlitCommandEncoder> blit;

	cbuf = [_commandQueue commandBuffer];

@autoreleasepool{
	id<CAMetalDrawable> drawable = [self nextDrawable];
	if(!drawable){
		LOG(@"display couldn't get drawable");
		[self setNeedsDisplay];
		return;
	}

	blit = [cbuf blitCommandEncoder];
	[blit copyFromTexture:_texture
		sourceSlice:0
		sourceLevel:0
		sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(_texture.width, _texture.height, _texture.depth)
		toTexture:drawable.texture
		destinationSlice:0
		destinationLevel:0
		destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];

	[cbuf presentDrawable:drawable];
	drawable = nil;
}
	[cbuf addCompletedHandler:^(id<MTLCommandBuffer> cmdBuff){
		if(cmdBuff.error){
			NSLog(@"command buffer finished with error: %@",
				cmdBuff.error.localizedDescription);
		}else{
			LOG(@"command buffer finished");
		}
	}];
	[cbuf commit];
}

@end
