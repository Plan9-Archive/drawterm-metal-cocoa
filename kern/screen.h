typedef struct Mouseinfo Mouseinfo;
typedef struct Mousestate Mousestate;
typedef struct Cursorinfo Cursorinfo;

struct Mousestate {
	Point	xy;
	int	buttons;
	ulong	counter;
	ulong	msec;
};

struct Mouseinfo {
	Lock		lk;
	Mousestate	state;
	ulong		lastcounter;
	int		resize;		/* generate resize event */
	Rendez		r;
	int		open;
	Mousestate	queue[16];	/* circular buffer of click events */
	ulong		ri;		/* read index into queue */
	ulong		wi;		/* write index into queue */
};

struct Cursorinfo {
	Lock	lk;
	Point	offset;
	uchar	clr[2*16];
	uchar	set[2*16];
};

extern	Memimage *gscreen;
extern	Mouseinfo mouse;
extern	Cursorinfo cursor;
extern	Cursorinfo arrow;

void	screeninit(void);
void	screenload(Rectangle, int, uchar *, Point, int);

void	getcolor(ulong, ulong*, ulong*, ulong*);
void	setcolor(ulong, ulong, ulong, ulong);

void	setcursor(void);
void	mouseset(Point);
void	flushmemscreen(Rectangle);
Memdata*attachscreen(Rectangle*, ulong*, int*, int*, int*);
void	deletescreenimage(void);
void	resetscreenimage(void);

extern	QLock drawlock;
#define	ishwimage(i)	0

void	terminit(void);
void	screenresize(Rectangle);
void	screensize(Rectangle, ulong);

void	mouseresize(void);
void	mousetrack(int, int, int, ulong);
void	absmousetrack(int, int, int, ulong);
void	cpubody(void);
void	guimain(void);
