/*
 * Manage tree of VtFiles stored in the block cache.
 *
 * The single point of truth for the info about the VtFiles themselves
 * is the block data.  Because of this, there is no explicit locking of
 * VtFile structures, and indeed there may be more than one VtFile
 * structure for a given Venti file.  They synchronize through the 
 * block cache.
 *
 * This is a bit simpler than fossil because there are no epochs
 * or tags or anything else.  Just mutable local blocks and immutable
 * Venti blocks.
 */

#include <u.h>
#include <libc.h>
#include <venti.h>

enum
{
	MaxBlock = (1UL<<31),
};

struct VtFile
{
	QLock lk;
	int ref;
	int local;
	VtBlock *b;			/* block containing this file */
	uchar score[VtScoreSize];	/* score of block containing this file */

/* immutable */
	VtCache *c;
	int mode;
	u32int gen;
	int dsize;
	int dir;
	VtFile *parent;
	int epb;			/* entries per block in parent */
	u32int offset; 			/* entry offset in parent */
};

static char EBadEntry[] = "bad VtEntry";
static char ENotDir[] = "walk in non-directory";
static char ETooBig[] = "file too big";
static char EBadAddr[] = "bad address";
static char ELabelMismatch[] = "label mismatch";

static int	sizetodepth(uvlong s, int psize, int dsize);
static VtBlock 	*fileload(VtFile *r, VtEntry *e);
static int	shrinkdepth(VtFile*, VtBlock*, VtEntry*, int);
static int	shrinksize(VtFile*, VtEntry*, uvlong);
static int	growdepth(VtFile*, VtBlock*, VtEntry*, int);

#define ISLOCKED(r)	((r)->b != nil)
#define DEPTH(t)	((t)&VtTypeDepthMask)

static VtFile *
vtfilealloc(VtCache *c, VtBlock *b, VtFile *p, u32int offset, int mode)
{
	int epb;
	u32int size;
	VtEntry e;
	VtFile *r;

	assert(p==nil || ISLOCKED(p));

	if(p == nil){
		assert(offset == 0);
		epb = 1;
	}else
		epb = p->dsize / VtEntrySize;

	if(b->type != VtDirType)
		goto Bad;

	/*
	 * a non-active entry is the only thing that
	 * can legitimately happen here. all the others
	 * get prints.
	 */
	if(vtentryunpack(&e, b->data, offset % epb) < 0){
		fprint(2, "vtentryunpack failed\n");
		goto Bad;
	}
	if(!(e.flags & VtEntryActive)){
		if(0)fprint(2, "not active\n");
		goto Bad;
	}
	if(e.psize < 256 || e.dsize < 256){
		fprint(2, "psize %ud dsize %ud\n", e.psize, e.dsize);
		goto Bad;
	}

	if(DEPTH(e.type) < sizetodepth(e.size, e.psize, e.dsize)){
		fprint(2, "depth %ud size %llud psize %ud dsize %ud\n",
			DEPTH(e.type), e.size, e.psize, e.dsize);
		goto Bad;
	}

	size = vtcacheblocksize(c);
	if(e.dsize > size || e.psize > size){
		fprint(2, "psize %ud dsize %ud blocksize %ud\n", e.psize, e.dsize, size);
		goto Bad;
	}

	r = vtmallocz(sizeof(VtFile));
	r->c = c;
	r->mode = mode;
	r->dsize = e.dsize;
	r->gen = e.gen;
	r->dir = (e.flags & VtEntryDir) != 0;
	r->ref = 1;
	r->parent = p;
	if(p){
		qlock(&p->lk);
		assert(mode == VtOREAD || p->mode == VtORDWR);
		p->ref++;
		qunlock(&p->lk);
	}
	memmove(r->score, b->score, VtScoreSize);
	r->offset = offset;
	r->epb = epb;

	return r;
Bad:
	werrstr(EBadEntry);
	return nil;
	
}

VtFile *
vtfileroot(VtCache *c, u32int addr, int mode)
{
	VtFile *r;
	VtBlock *b;

	b = vtcachelocal(c, addr, VtDirType);
	if(b == nil)
		return nil;

	r = vtfilealloc(c, b, nil, 0, mode);
	vtblockput(b);
	return r;
}

VtFile*
vtfileopenroot(VtCache *c, VtEntry *e)
{
	VtBlock *b;
	VtFile *f;

	b = vtcacheallocblock(c, VtDirType);
	if(b == nil)
		return nil;

	vtentrypack(e, b->data, 0);
	f = vtfilealloc(c, b, nil, 0, VtORDWR);
	vtblockput(b);
	return f;
}

VtFile *
vtfilecreateroot(VtCache *c, int psize, int dsize, int type)
{
	VtEntry e;

	memset(&e, 0, sizeof e);
	e.flags = VtEntryActive;
	e.psize = psize;
	e.dsize = dsize;
	if(type == VtDirType)
		e.flags |= VtEntryDir;
	memmove(e.score, vtzeroscore, VtScoreSize);

	return vtfileopenroot(c, &e);
}

VtFile *
vtfileopen(VtFile *r, u32int offset, int mode)
{
	ulong bn;
	VtBlock *b;

	assert(ISLOCKED(r));
	if(!r->dir){
		werrstr(ENotDir);
		return nil;
	}

	bn = offset/(r->dsize/VtEntrySize);

	b = vtfileblock(r, bn, mode);
	if(b == nil)
		return nil;
	r = vtfilealloc(r->c, b, r, offset, mode);
	vtblockput(b);
	return r;
}

VtFile *
vtfilecreate(VtFile *r, int psize, int dsize, int dir)
{
	int i;
	VtBlock *b;
	u32int bn, size;
	VtEntry e;
	int epb;
	VtFile *rr;
 	u32int offset;

	assert(ISLOCKED(r));

	if(!r->dir){
		werrstr(ENotDir);
		return nil;
	}

	epb = r->dsize/VtEntrySize;

	size = vtfilegetdirsize(r);
	/*
	 * look at a random block to see if we can find an empty entry
	 */
	offset = lnrand(size+1);
	offset -= offset % epb;

	/* try the given block and then try the last block */
	for(;;){
		bn = offset/epb;
		b = vtfileblock(r, bn, VtORDWR);
		if(b == nil)
			return nil;
		for(i=offset%r->epb; i<epb; i++){
			if(vtentryunpack(&e, b->data, i) < 0)
				continue;
			if((e.flags&VtEntryActive) == 0 && e.gen != ~0)
				goto Found;
		}
		vtblockput(b);
		if(offset == size){
			fprint(2, "vtfilecreate: cannot happen\n");
			werrstr("vtfilecreate: cannot happen");
			return nil;
		}
		offset = size;
	}

Found:
	/* found an entry - gen already set */
	e.psize = psize;
	e.dsize = dsize;
	e.flags = VtEntryActive;
	e.type = dir ? VtDirType : VtDataType;
	e.size = 0;
	memmove(e.score, vtzeroscore, VtScoreSize);
	vtentrypack(&e, b->data, i);

	offset = bn*epb + i;
	if(offset+1 > size){
		if(vtfilesetdirsize(r, offset+1) < 0){
			vtblockput(b);
			return nil;
		}
	}

	rr = vtfilealloc(r->c, b, r, offset, VtORDWR);
	vtblockput(b);
	return rr;
}

static int
vtfilekill(VtFile *r, int doremove)
{
	VtEntry e;
	VtBlock *b;

	assert(ISLOCKED(r));
	b = fileload(r, &e);
	if(b == nil)
		return -1;

	if(doremove==0 && e.size == 0){
		/* already truncated */
		vtblockput(b);
		return 0;
	}

	if(doremove){
		if(e.gen != ~0)
			e.gen++;
		e.dsize = 0;
		e.psize = 0;
		e.flags = 0;
	}else
		e.flags &= ~VtEntryLocal;
	e.type = 0;
	e.size = 0;
	memmove(e.score, vtzeroscore, VtScoreSize);
	vtentrypack(&e, b->data, r->offset % r->epb);
	vtblockput(b);

	if(doremove){
		vtfileunlock(r);
		vtfileclose(r);
	}

	return 0;
}

int
vtfileremove(VtFile *r)
{
	return vtfilekill(r, 1);
}

int
vtfiletruncate(VtFile *r)
{
	return vtfilekill(r, 0);
}

uvlong
vtfilegetsize(VtFile *r)
{
	VtEntry e;
	VtBlock *b;

	assert(ISLOCKED(r));
	b = fileload(r, &e);
	if(b == nil)
		return ~(uvlong)0;
	vtblockput(b);

	return e.size;
}

static int
shrinksize(VtFile *r, VtEntry *e, uvlong size)
{
	int i, depth, type, isdir, ppb;
	uvlong ptrsz;
	uchar score[VtScoreSize];
	VtBlock *b;

	b = vtcacheglobal(r->c, e->score, e->type);
	if(b == nil)
		return -1;

	ptrsz = e->dsize;
	ppb = e->psize/VtScoreSize;
	type = e->type;
	depth = DEPTH(type);
	for(i=0; i+1<depth; i++)
		ptrsz *= ppb;

	isdir = r->dir;
	while(depth > 0){
		if(b->addr == NilBlock){
			/* not worth copying the block just so we can zero some of it */
			vtblockput(b);
			return -1;
		}

		/*
		 * invariant: each pointer in the tree rooted at b accounts for ptrsz bytes
		 */

		/* zero the pointers to unnecessary blocks */
		i = (size+ptrsz-1)/ptrsz;
		for(; i<ppb; i++)
			memmove(b->data+i*VtScoreSize, vtzeroscore, VtScoreSize);

		/* recurse (go around again) on the partially necessary block */
		i = size/ptrsz;
		size = size%ptrsz;
		if(size == 0){
			vtblockput(b);
			return 0;
		}
		ptrsz /= ppb;
		type--;
		memmove(score, b->data+i*VtScoreSize, VtScoreSize);
		vtblockput(b);
		b = vtcacheglobal(r->c, score, type);
		if(b == nil)
			return -1;
	}

	if(b->addr == NilBlock){
		vtblockput(b);
		return -1;
	}

	/*
	 * No one ever truncates BtDir blocks.
	 */
	if(depth==0 && !isdir && e->dsize > size)
		memset(b->data+size, 0, e->dsize-size);
	vtblockput(b);
	return 0;
}

int
vtfilesetsize(VtFile *r, uvlong size)
{
	int depth, edepth;
	VtEntry e;
	VtBlock *b;

	assert(ISLOCKED(r));
	if(size == 0)
		return vtfiletruncate(r);

	if(size > VtMaxFileSize || size > ((uvlong)MaxBlock)*r->dsize){
		werrstr(ETooBig);
		return -1;
	}

	b = fileload(r, &e);
	if(b == nil)
		return -1;

	/* quick out */
	if(e.size == size){
		vtblockput(b);
		return 0;
	}

	depth = sizetodepth(size, e.psize, e.dsize);
	edepth = DEPTH(e.type);
	if(depth < edepth){
		if(shrinkdepth(r, b, &e, depth) < 0){
			vtblockput(b);
			return -1;
		}
	}else if(depth > edepth){
		if(growdepth(r, b, &e, depth) < 0){
			vtblockput(b);
			return -1;
		}
	}

	if(size < e.size)
		shrinksize(r, &e, size);

	e.size = size;
	vtentrypack(&e, b->data, r->offset % r->epb);
	vtblockput(b);

	return 0;
}

int
vtfilesetdirsize(VtFile *r, u32int ds)
{
	uvlong size;
	int epb;

	assert(ISLOCKED(r));
	epb = r->dsize/VtEntrySize;

	size = (uvlong)r->dsize*(ds/epb);
	size += VtEntrySize*(ds%epb);
	return vtfilesetsize(r, size);
}

u32int
vtfilegetdirsize(VtFile *r)
{
	ulong ds;
	uvlong size;
	int epb;

	assert(ISLOCKED(r));
	epb = r->dsize/VtEntrySize;

	size = vtfilegetsize(r);
	ds = epb*(size/r->dsize);
	ds += (size%r->dsize)/VtEntrySize;
	return ds;
}

int
vtfilegetentry(VtFile *r, VtEntry *e)
{
	VtBlock *b;

	assert(ISLOCKED(r));
	b = fileload(r, e);
	if(b == nil)
		return -1;
	vtblockput(b);

	return 0;
}

int
vtfilesetentry(VtFile *r, VtEntry *e)
{
	VtBlock *b;
	VtEntry ee;

	assert(ISLOCKED(r));
	b = fileload(r, &ee);
	if(b == nil)
		return -1;
	vtentrypack(e, b->data, r->offset % r->epb);
	vtblockput(b);
	return 0;
}

static VtBlock *
blockwalk(VtBlock *p, int index, VtCache *c, int mode, VtEntry *e)
{
	VtBlock *b;
	int type;
	uchar *score;
	VtEntry oe;

	switch(p->type){
	case VtDataType:
		assert(0);
	case VtDirType:
		type = e->type;
		score = e->score;
		break;
	default:
		type = p->type - 1;
		score = p->data+index*VtScoreSize;
		break;
	}
//print("walk from %V/%d ty %d to %V ty %d\n", p->score, index, p->type, score, type);

	if(mode == VtOWRITE && vtglobaltolocal(score) == NilBlock){
		b = vtcacheallocblock(c, type);
		if(b)
			goto HaveCopy;
	}else
		b = vtcacheglobal(c, score, type);

	if(b == nil || mode == VtOREAD)
		return b;

	if(vtglobaltolocal(b->score) != NilBlock)
		return b;

	oe = *e;

	/*
	 * Copy on write.
	 */
	e->flags |= VtEntryLocal;

	b = vtblockcopy(b/*, e->tag, fs->ehi, fs->elo*/);
	if(b == nil)
		return nil;

HaveCopy:
	if(p->type == VtDirType){
		memmove(e->score, b->score, VtScoreSize);
		vtentrypack(e, p->data, index);
	}else{
		memmove(p->data+index*VtScoreSize, b->score, VtScoreSize);
	}
	return b;
}

/*
 * Change the depth of the VtFile r.
 * The entry e for r is contained in block p.
 */
static int
growdepth(VtFile *r, VtBlock *p, VtEntry *e, int depth)
{
	VtBlock *b, *bb;
	VtEntry oe;

	assert(ISLOCKED(r));
	assert(depth <= VtPointerDepth);

	b = vtcacheglobal(r->c, e->score, e->type);
	if(b == nil)
		return -1;

	oe = *e;

	/*
	 * Keep adding layers until we get to the right depth
	 * or an error occurs.
	 */
	while(DEPTH(e->type) < depth){
		bb = vtcacheallocblock(r->c, e->type+1);
		if(bb == nil)
			break;
		memmove(bb->data, b->score, VtScoreSize);
		memmove(e->score, bb->score, VtScoreSize);	
		e->type++;
		e->flags |= VtEntryLocal;
		vtblockput(b);
		b = bb;
	}

	vtentrypack(e, p->data, r->offset % r->epb);
	vtblockput(b);

	if(DEPTH(e->type) == depth)
		return 0;
	return -1;
}

static int
shrinkdepth(VtFile *r, VtBlock *p, VtEntry *e, int depth)
{
	VtBlock *b, *nb, *ob, *rb;
	VtEntry oe;

	assert(ISLOCKED(r));
	assert(depth <= VtPointerDepth);

	rb = vtcacheglobal(r->c, e->score, e->type);
	if(rb == nil)
		return 0;

	/*
	 * Walk down to the new root block.
	 * We may stop early, but something is better than nothing.
	 */
	oe = *e;

	ob = nil;
	b = rb;
	for(; DEPTH(e->type) > depth; e->type--){
		nb = vtcacheglobal(r->c, b->data, e->type-1);
		if(nb == nil)
			break;
		if(ob!=nil && ob!=rb)
			vtblockput(ob);
		ob = b;
		b = nb;
	}

	if(b == rb){
		vtblockput(rb);
		return 0;
	}

	/*
	 * Right now, e points at the root block rb, b is the new root block,
	 * and ob points at b.  To update:
	 *
	 *	(i) change e to point at b
	 *	(ii) zero the pointer ob -> b
	 *	(iii) free the root block
	 *
	 * p (the block containing e) must be written before
	 * anything else.
 	 */

	/* (i) */
	memmove(e->score, b->score, VtScoreSize);
	vtentrypack(e, p->data, r->offset % r->epb);

	/* (ii) */
	memmove(ob->data, vtzeroscore, VtScoreSize);

	/* (iii) */
	vtblockput(rb);
	if(ob!=nil && ob!=rb)
		vtblockput(ob);
	vtblockput(b);

	if(DEPTH(e->type) == depth)
		return 0;
	return -1;
}

static int
mkindices(VtEntry *e, u32int bn, int *index)
{
	int i, np;

	memset(index, 0, VtPointerDepth*sizeof(int));

	np = e->psize/VtScoreSize;
	for(i=0; bn > 0; i++){
		if(i >= VtPointerDepth){
			werrstr(EBadAddr);
			return -1;
		}
		index[i] = bn % np;
		bn /= np;
	}
	return i;
}
	
VtBlock *
vtfileblock(VtFile *r, u32int bn, int mode)
{
	VtBlock *b, *bb;
	int index[VtPointerDepth+1];
	VtEntry e;
	int i;
	int m;

	assert(ISLOCKED(r));
	assert(bn != NilBlock);

	b = fileload(r, &e);
	if(b == nil)
		return nil;

	i = mkindices(&e, bn, index);
	if(i < 0)
		return nil;
	if(i > DEPTH(e.type)){
		if(mode == VtOREAD){
			werrstr(EBadAddr);
			goto Err;
		}
		index[i] = 0;
		if(growdepth(r, b, &e, i) < 0)
			goto Err;
	}

assert(b->type == VtDirType);

	index[DEPTH(e.type)] = r->offset % r->epb;

	/* mode for intermediate block */
	m = mode;
	if(m == VtOWRITE)
		m = VtORDWR;

	for(i=DEPTH(e.type); i>=0; i--){
		bb = blockwalk(b, index[i], r->c, i==0 ? mode : m, &e);
		if(bb == nil)
			goto Err;
		vtblockput(b);
		b = bb;
	}
	return b;
Err:
	vtblockput(b);
	return nil;
}

int
vtfileblockhash(VtFile *r, u32int bn, uchar score[VtScoreSize])
{
	VtBlock *b, *bb;
	int index[VtPointerDepth+1];
	VtEntry e;
	int i;

	assert(ISLOCKED(r));
	assert(bn != NilBlock);

	b = fileload(r, &e);
	if(b == nil)
		return -1;

	i = mkindices(&e, bn, index);
	if(i < 0){
		vtblockput(b);
		return -1;
	}
	if(i > DEPTH(e.type)){
		memmove(score, vtzeroscore, VtScoreSize);
		vtblockput(b);
		return 0;
	}

	index[DEPTH(e.type)] = r->offset % r->epb;

	for(i=DEPTH(e.type); i>=1; i--){
		bb = blockwalk(b, index[i], r->c, VtOREAD, &e);
		if(bb == nil)
			goto Err;
		vtblockput(b);
		b = bb;
		if(memcmp(b->score, vtzeroscore, VtScoreSize) == 0)
			break;
	}

	memmove(score, b->data+index[0]*VtScoreSize, VtScoreSize);
	vtblockput(b);
	return 0;

Err:
fprint(2, "vtfileblockhash: %r\n");
	vtblockput(b);
	return -1;
}

void
vtfileincref(VtFile *r)
{
	qlock(&r->lk);
	r->ref++;
	qunlock(&r->lk);
}

void
vtfileclose(VtFile *r)
{
	if(r == nil)
		return;
	qlock(&r->lk);
	r->ref--;
	if(r->ref){
		qunlock(&r->lk);
		return;
	}
	assert(r->ref == 0);
	qunlock(&r->lk);
	if(r->parent)
		vtfileclose(r->parent);
	memset(r, ~0, sizeof(*r));
	vtfree(r);
}

/*
 * Retrieve the block containing the entry for r.
 * If a snapshot has happened, we might need
 * to get a new copy of the block.  We avoid this
 * in the common case by caching the score for
 * the block and the last epoch in which it was valid.
 *
 * We use r->mode to tell the difference between active
 * file system VtFiles (VtORDWR) and VtFiles for the
 * snapshot file system (VtOREAD).
 */
static VtBlock*
fileloadblock(VtFile *r, int mode)
{
	char e[ERRMAX];
	u32int addr;
	VtBlock *b;

	switch(r->mode){
	default:
		assert(0);
	case VtORDWR:
		assert(r->mode == VtORDWR);
		if(r->local == 1){
			b = vtcacheglobal(r->c, r->score, VtDirType);
			if(b == nil)
				return nil;
			return b;
		}
		assert(r->parent != nil);
		if(vtfilelock(r->parent, VtORDWR) < 0)
			return nil;
		b = vtfileblock(r->parent, r->offset/r->epb, VtORDWR);
		vtfileunlock(r->parent);
		if(b == nil)
			return nil;
		memmove(r->score, b->score, VtScoreSize);
		r->local = 1;
		return b;

	case VtOREAD:
		if(mode == VtORDWR){
			werrstr("read/write lock of read-only file");
			return nil;
		}
		addr = vtglobaltolocal(r->score);
		if(addr == NilBlock)
			return vtcacheglobal(r->c, r->score, VtDirType);

		b = vtcachelocal(r->c, addr, VtDirType);
		if(b)
			return b;

		/*
		 * If it failed because the epochs don't match, the block has been
		 * archived and reclaimed.  Rewalk from the parent and get the
		 * new pointer.  This can't happen in the VtORDWR case
		 * above because blocks in the current epoch don't get
		 * reclaimed.  The fact that we're VtOREAD means we're
		 * a snapshot.  (Or else the file system is read-only, but then
		 * the archiver isn't going around deleting blocks.)
		 */
		rerrstr(e, sizeof e);
		if(strcmp(e, ELabelMismatch) == 0){
			if(vtfilelock(r->parent, VtOREAD) < 0)
				return nil;
			b = vtfileblock(r->parent, r->offset/r->epb, VtOREAD);
			vtfileunlock(r->parent);
			if(b){
				fprint(2, "vtfilealloc: lost %V found %V\n",
					r->score, b->score);
				memmove(r->score, b->score, VtScoreSize);
				return b;
			}
		}
		return nil;
	}
}

int
vtfilelock(VtFile *r, int mode)
{
	VtBlock *b;

	if(mode == -1)
		mode = r->mode;

	b = fileloadblock(r, mode);
	if(b == nil)
		return -1;
	/*
	 * The fact that we are holding b serves as the
	 * lock entitling us to write to r->b.
	 */
	assert(r->b == nil);
	r->b = b;
	return 0;
}

/*
 * Lock two (usually sibling) VtFiles.  This needs special care
 * because the Entries for both vtFiles might be in the same block.
 * We also try to lock blocks in left-to-right order within the tree.
 */
int
vtfilelock2(VtFile *r, VtFile *rr, int mode)
{
	VtBlock *b, *bb;

	if(rr == nil)
		return vtfilelock(r, mode);

	if(mode == -1)
		mode = r->mode;

	if(r->parent==rr->parent && r->offset/r->epb == rr->offset/rr->epb){
		b = fileloadblock(r, mode);
		if(b == nil)
			return -1;
		vtblockduplock(b);
		bb = b;
	}else if(r->parent==rr->parent || r->offset > rr->offset){
		bb = fileloadblock(rr, mode);
		b = fileloadblock(r, mode);
	}else{
		b = fileloadblock(r, mode);
		bb = fileloadblock(rr, mode);
	}
	if(b == nil || bb == nil){
		if(b)
			vtblockput(b);
		if(bb)
			vtblockput(bb);
		return -1;
	}

	/*
	 * The fact that we are holding b and bb serves
	 * as the lock entitling us to write to r->b and rr->b.
	 */
	r->b = b;
	rr->b = bb;
	return 0;
}

void
vtfileunlock(VtFile *r)
{
	VtBlock *b;

	if(r->b == nil){
		fprint(2, "vtfileunlock: already unlocked\n");
		abort();
	}
	b = r->b;
	r->b = nil;
	vtblockput(b);
}

static VtBlock*
fileload(VtFile *r, VtEntry *e)
{
	VtBlock *b;

	assert(ISLOCKED(r));
	b = r->b;
	if(vtentryunpack(e, b->data, r->offset % r->epb) < 0)
		return nil;
	vtblockduplock(b);
	return b;
}

static int
sizetodepth(uvlong s, int psize, int dsize)
{
	int np;
	int d;
	
	/* determine pointer depth */
	np = psize/VtScoreSize;
	s = (s + dsize - 1)/dsize;
	for(d = 0; s > 1; d++)
		s = (s + np - 1)/np;
	return d;
}

long
vtfileread(VtFile *f, void *data, long count, vlong offset)
{
	int frag;
	VtBlock *b;
	VtEntry e;

	assert(ISLOCKED(f));

	vtfilegetentry(f, &e);
	if(count == 0)
		return 0;
	if(count < 0 || offset < 0){
		werrstr("vtfileread: bad offset or count");
		return -1;
	}
	if(offset >= e.size)
		return 0;

	if(offset+count > e.size)
		count = e.size - offset;

	frag = offset % e.dsize;
	if(frag+count > e.dsize)
		count = e.dsize - frag;

	b = vtfileblock(f, offset/e.dsize, VtOREAD);
	if(b == nil)
		return -1;

	memmove(data, b->data+frag, count);
	vtblockput(b);
	return count;
}

static long
filewrite1(VtFile *f, void *data, long count, vlong offset)
{
	int frag, m;
	VtBlock *b;
	VtEntry e;

	vtfilegetentry(f, &e);
	if(count < 0 || offset < 0){
		werrstr("vtfilewrite: bad offset or count");
		return -1;
	}

	frag = offset % e.dsize;
	if(frag+count > e.dsize)
		count = e.dsize - frag;

	m = VtORDWR;
	if(frag == 0 && count == e.dsize)
		m = VtOWRITE;

	b = vtfileblock(f, offset/e.dsize, m);
	if(b == nil)
		return -1;

	memmove(b->data+frag, data, count);

	if(offset+count > e.size){
		vtfilegetentry(f, &e);
		e.size = offset+count;
		vtfilesetentry(f, &e);
	}

	vtblockput(b);
	return count;
}

long
vtfilewrite(VtFile *f, void *data, long count, vlong offset)
{
	long tot, m;

	assert(ISLOCKED(f));

	tot = 0;
	m = 0;
	while(tot < count){
		m = filewrite1(f, (char*)data+tot, count-tot, offset+tot);
		if(m <= 0)
			break;
		tot += m;
	}
	if(tot==0)
		return m;
	return tot;
}

static int
flushblock(VtCache *c, VtBlock *bb, uchar score[VtScoreSize], int ppb, int epb,
	int type)
{
	u32int addr;
	VtBlock *b;
	VtEntry e;
	int i;

	addr = vtglobaltolocal(score);
	if(addr == NilBlock)
		return 0;

	if(bb){
		b = bb;
		if(memcmp(b->score, score, VtScoreSize) != 0)
			abort();
	}else
		if((b = vtcachelocal(c, addr, type)) == nil)
			return -1;

	switch(type){
	case VtDataType:
		break;

	case VtDirType:
		for(i=0; i<epb; i++){
			if(vtentryunpack(&e, b->data, i) < 0)
				goto Err;
			if(flushblock(c, nil, e.score, e.psize/VtScoreSize, e.dsize/VtEntrySize,
				e.type) < 0)
				goto Err;
		}
		break;
	
	default:	/* VtPointerTypeX */
		for(i=0; i<ppb; i++){
			if(flushblock(c, nil, b->data+VtScoreSize*i, ppb, epb, type-1) < 0)
				goto Err;
		}
		break;
	}

	if(vtblockwrite(b) < 0)
		goto Err;
	memmove(score, b->score, VtScoreSize);
	if(b != bb)
		vtblockput(b);
	return 0;

Err:
	if(b != bb)
		vtblockput(b);
	return -1;
}

int
vtfileflush(VtFile *f)
{
	int ret;
	VtBlock *b;
	VtEntry e;

	assert(ISLOCKED(f));
	b = fileload(f, &e);
	if(!(e.flags&VtEntryLocal)){
		vtblockput(b);
		return 0;
	}

	ret = flushblock(f->c, nil, e.score, e.psize/VtScoreSize, e.dsize/VtEntrySize,
		e.type);
	if(!ret){
		vtblockput(b);
		return -1;
	}

	vtentrypack(&e, b->data, f->offset % f->epb);
	vtblockput(b);
	return 0;
}

int
vtfileflushbefore(VtFile *r, u64int offset)
{
	VtBlock *b, *bb;
	VtEntry e;
	int i, base, depth, ppb, epb, ok;
	int index[VtPointerDepth+1], index1[VtPointerDepth+1], j, ret;
	VtBlock *bi[VtPointerDepth+2];
	uchar *score;

	assert(ISLOCKED(r));
	if(offset == 0)
		return 0;

	b = fileload(r, &e);
	if(b == nil)
		return -1;

	ret = -1;
	memset(bi, 0, sizeof bi);
	depth = DEPTH(e.type);
	bi[depth+1] = b;
	i = mkindices(&e, (offset-1)/e.dsize, index);
	if(i < 0)
		goto Err;
	if(i > depth)
		goto Err;
	mkindices(&e, offset/e.dsize, index1);
	ppb = e.psize / VtScoreSize;
	epb = e.dsize / VtEntrySize;

	index[depth] = r->offset % r->epb;
	for(i=depth; i>=0; i--){
		bb = blockwalk(b, index[i], r->c, VtORDWR, &e);
		if(bb == nil)
			goto Err;
		bi[i] = bb;
		b = bb;
	}
	ret = 0;

	base = e.type&~VtTypeDepthMask;
	for(i=0; i<depth; i++){
		if(i == 0){
			/* bottom: data or dir block */
			ok = offset%e.dsize == 0;
		}else{
			/* middle: pointer blocks */
			b = bi[i];
			/*
			 * flush everything up to the break
			 */
			for(j=0; j<index[i-1]; j++)
				if(flushblock(r->c, nil, b->data+j*VtScoreSize, ppb, epb, base+i-1) < 0)
					goto Err;
			/*
			 * if the rest of the block is already flushed,
			 * we can flush the whole block.
			 */
			ok = 1;
			for(; j<ppb; j++)
				if(vtglobaltolocal(b->data+j*VtScoreSize) != NilBlock)
					ok = 0;
		}
		if(ok){
			if(i == depth)
				score = e.score;
			else
				score = bi[i+1]->data+index[i]*VtScoreSize;
			if(flushblock(r->c, bi[i], score, ppb, epb, base+i) < 0)
				goto Err;
		}
	}

Err:
	/* top: entry.  do this always so that the score is up-to-date */
	vtentrypack(&e, bi[depth+1]->data, index[depth]);
	for(i=0; i<nelem(bi); i++)
		if(bi[i])
			vtblockput(bi[i]);
	return ret;
}
