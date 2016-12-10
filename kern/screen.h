typedef struct Mouseinfo Mouseinfo;
typedef struct Mousestate Mousestate;
typedef struct Cursorinfo Cursorinfo;
typedef struct Screeninfo Screeninfo;

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

struct Screeninfo {
	Lock	lk;
	int	depth;
	int	dibtype;
};

extern	Memimage *gscreen;
extern	Mouseinfo mouse;
extern	Cursorinfo cursor;
extern	Cursorinfo arrow;
extern	Screeninfo screen;

void	screeninit(void);
void	screenload(Rectangle, int, uchar *, Point, int);

void	getcolor(ulong, ulong*, ulong*, ulong*);
void	setcolor(ulong, ulong, ulong, ulong);

void	setcursor(void);
void	mouseset(Point);
void	flushmemscreen(Rectangle);
uchar*	attachscreen(Rectangle*, ulong*, int*, int*, int*);

extern	QLock drawlock;
#define	ishwimage(i)	0

void	terminit(void);

void	absmousetrack(int, int, int, int);
