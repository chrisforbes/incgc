#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/*
 * arena is 64K naturally aligned. allocation unit is 16B.
 * due to natural alignment, if you have a ptr *into* an arena,
 * you can recover the arena ptr itself by just masking the low
 * 16 bits.
 */

#define ARENA_SIZE (64*1024)
#define ALLOC_UNIT (16)
/* TODO: 32/64 */
#define GS_SIZE (510)

#define __noreturn __attribute__((noreturn))
#define __pure __attribute__((pure))

void __noreturn die(int exitcode, char const * p, ...) {
	va_list vl;
	va_start( vl, p );
	vfprintf( stderr, p, vl );
	fputc( '\n', stderr );
	fflush( stderr );
	exit(exitcode);
}

/* header of an object on the gc heap */
struct obj {
	unsigned char gray:1;
	unsigned char type:7;
};

/* because the used/mark arrays are at the start of the arena, 
 * we don't actually allocate the first N allocation units. this
 * allows the corresponding bits in the free/mark arrays to be
 * reused for other metadata.
 *
 * these structs contain that metadata, and must not grow larger
 * than 16 bytes each.
 */
struct arena_meta_a { int nextcell; struct gs * gs; };
struct arena_meta_b { int dummy; };

struct check_sizes {
	int meta_a_too_big[ 16 - (int)sizeof( struct arena_meta_a ) ];
	int meta_b_too_big[ 16 - (int)sizeof( struct arena_meta_b ) ];
};

struct arena {
	union {
		int used[ARENA_SIZE / ALLOC_UNIT / sizeof(int) / 8];
		struct arena_meta_a a;
	};
	union {
		int mark[ARENA_SIZE / ALLOC_UNIT / sizeof(int) / 8];
		struct arena_meta_b b;
	};
	/* 64K - sizeof(struct arena) of data follows */
};

struct arena * arena_new(void) {
	struct arena * a = 0;
	if (posix_memalign( (void **) &a, ARENA_SIZE, ARENA_SIZE ))
		die( 1, "arena allocation failed" );

	memset( a, 0, sizeof(*a) );
	a->a.nextcell = (sizeof(struct arena) + ALLOC_UNIT - 1) >> 4;

	return a;
}

struct obj * arena_alloc(struct arena * a, size_t objsize) {
	if (objsize < sizeof(struct obj))
		return 0; /* can't allocate less than a gc header */

	int numunits = (objsize + ALLOC_UNIT - 1) >> 4;

	/* TODO: add first-fit or best-fit strategies when
	 * there is significant fragmentation pressure */
	if (a->a.nextcell + numunits > (ARENA_SIZE >> 4))
		return 0;	/* no room */

	struct obj * o = (struct obj *)((size_t)a + (a->a.nextcell << 4));
	o->gray = 0;	/* new objects white? */
	a->a.nextcell += numunits;

	return 0;
}

/* due to arenas being aligned to 64K, we can just mask off
 * these bits */
static inline struct arena * get_arena(struct obj * o) {
	size_t s = (size_t) o;
	return (struct arena *)( s & ~((size_t)0x0ffff) );
}

/* gray stacks ----------------------------------------------------------- */

struct gs {
	int n;
	struct gs * prev;
	struct obj * data[GS_SIZE];
};

/* stash of a few gs chunks, to avoid churning the system
 * allocator during marking */
static struct gs * spare_gs = 0;

/* get a gs chunk from the stash, or alloc a new one. */
static inline struct gs * gs_get(void) {
	if (!spare_gs)
		return malloc(sizeof(struct gs));

	struct gs * gs = spare_gs;
	spare_gs = gs->prev;
	return gs;
}

/* put a gs chunk back into the stash */
static inline void gs_put(struct gs * gs) {
	gs->prev = spare_gs;
	spare_gs = gs->prev;
}

/* push an object onto its arena's gs */
static inline void gs_push(struct arena * a, struct obj * o) {
	if (!a->a.gs || a->a.gs->n == GS_SIZE) {
		struct gs * prev = a->a.gs;
		a->a.gs = gs_get();
		a->a.gs->n = 0;
		a->a.gs->prev = prev;
	}

	struct gs * gs = a->a.gs;
	gs->data[ GS_SIZE - ++gs->n ] = o;
}

/* pop the first object off an arena's gs */
static inline struct obj * gs_pop(struct arena * a) {
	struct gs * gs = a->a.gs;
	if (!gs) return 0;
	struct obj * o = gs->data[ GS_SIZE - gs->n-- ];
	if (!gs->n) {
		a->a.gs = gs->prev;
		gs_put(gs);
	}
	return o;
}

/* real meat ------------------------------------------------------------- */

#define IS_MARKED(a,c)\
	((a)->mark[(c) >> 5] & ((c) & 0x1f))
#define MARK(a,c)\
	do { (a)->mark[(c) >> 5] |= ((c) & 0x1f); } while(0)

/* after writing a ptr field in an object, reachability can change, so make
 * the object gray. if it is now dark-gray, push it back onto the gs for its
 * arena. */
void write_barrier(struct obj * o) {
	if (o->gray) return;
	o->gray = 1;
	struct arena * a = get_arena(o);
	size_t cell = (size_t)o - (size_t)a;
	if (IS_MARKED(a, cell))
		gs_push(a, o);
}

/* pop the first object off an arena's gs, and mark it. */
int mark(struct arena * a) {
	struct obj * o = gs_pop(a);
	if (!o) return 0;

	/* make it black */
	size_t cell = (size_t)o - (size_t)a;
	MARK(a, cell);
	o->gray = 0;

	/* TODO: walk pointers from this object, and
	 * push them onto their arena's gs.
	 *
	 * This is heavily dependent on the structure of the actual
	 * objects!
	 * */

	return 1;
}

/* sweep away all unmarked objects remaining in an arena */
void sweep(struct arena * a) {
	if (a->a.gs && a->a.gs->n)
		die( 1, "broken GC: arena %p had things remaining to mark", a);

	size_t i;
	for( i = sizeof(*a) >> 9; i < sizeof(a->used) / sizeof(*a->used); i++ ) {
		int mark = a->mark[i];
		int used = a->used[i];

		a->mark[i] = used & mark;
		a->used[i] = used ^ mark;
	}
}

/* test driver code ------------------------------------------------------ */

int main(void) {
	printf( "sizes: arena meta: %zd a: %zd b: %zd: gs: %zd\n",
			sizeof(struct arena),
			sizeof(struct arena_meta_a),
			sizeof(struct arena_meta_b),
			sizeof(struct gs));

	struct arena * a = arena_new();
	struct obj * o = arena_alloc(a, 32);

	if (!o)
		die( 1, "failed: alloc object from %p", a );

	return 0;
}
