typedef enum Vis{
	None=0,
	Some,
	All,
}Vis;

enum{
	Clicktime=500,		/* milliseconds */
};

typedef struct Flayer Flayer;

struct Flayer
{
	Frame		f;
	long		origin;	/* offset of first char in flayer */
	long		p0, p1;
	long		click;	/* time at which selection click occurred, in HZ */
	Rune		*(*textfn)(Flayer*, long, ulong*);
	int		user0;
	void		*user1;
	Rectangle	entire;
	Rectangle	scroll;
	Rectangle	lastsr;	/* geometry of scrollbar when last drawn */
	Vis		visible;
};

void	flborder(Flayer*, int);
void	flclose(Flayer*);
void	fldelete(Flayer*, long, long);
void	flfp0p1(Flayer*, ulong*, ulong*);
void	flinit(Flayer*, Rectangle, Font*, Image**);
void	flinsert(Flayer*, Rune*, Rune*, long);
void	flnew(Flayer*, Rune *(*fn)(Flayer*, long, ulong*), int, void*);
int	flprepare(Flayer*);
Rectangle flrect(Flayer*, Rectangle);
void	flrefresh(Flayer*, Rectangle, int);
void	flresize(Rectangle);
int	flselect(Flayer*, ulong*);
void	flsetselect(Flayer*, long, long);
void	flstart(Rectangle);
void	flupfront(Flayer*);
Flayer	*flwhich(Point);
int     flscale(Flayer*, int);

#define	FLMARGIN(l)	flscale(l, 4)
#define	FLSCROLLWID(l)	flscale(l, 12)
#define	FLGAP(l)		flscale(l, 4)

extern	Image	*maincols[NCOL];
extern	Image	*cmdcols[NCOL];
