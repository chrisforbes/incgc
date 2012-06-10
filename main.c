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
#define __unused __attribute__((unused))

void __noreturn die(int exitcode, char const * p, ...) {
	va_list vl;
	va_start( vl, p );
	vfprintf( stderr, p, vl );
	fputc( '\n', stderr );
	fflush( stderr );
	exit(exitcode);
}

struct gs {
	int n;
	struct gs * prev;
	struct obj * data[GS_SIZE];
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

/*
 * An actual object in the GC heap. Always in an arena!
 */
struct obj {
	unsigned char gray:1;
	unsigned char type:7;
};

struct arena * arena_new(void) {
	struct arena * a = 0;
	if (posix_memalign( (void **) &a, ARENA_SIZE, ARENA_SIZE ))
		die( 1, "arena allocation failed" );

	memset( a, 0, sizeof(*a) );
	a->a.nextcell = (sizeof(struct arena) + ALLOC_UNIT - 1) >> 4;

	return a;
}

struct obj * arena_alloc(struct arena * a, int objsize) {
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

/* stash of graystacks we're not using currently */
static struct gs * spare_gs = 0;

static inline struct gs * gs_get(void) {
	if (!spare_gs)
		return malloc(sizeof(struct gs));

	struct gs * gs = spare_gs;
	spare_gs = gs->prev;
	return gs;
}

static inline void gs_put(struct gs * gs) {
	gs->prev = spare_gs;
	spare_gs = gs->prev;
}

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

void write_barrier(struct obj * o) {
	if (o->gray) return;
	o->gray = 1;
	struct arena * a = get_arena(o);
	size_t cell = (size_t)o - (size_t)a;
	if (a->mark[cell >> 5] & (cell & 0x1f))
		gs_push(a, o);
}

void mark(struct arena * a) {
	struct obj * o = gs_pop(a);
	if (!o) return;

	/* make it black */
	size_t cell = (size_t)o - (size_t)a;
	a->mark[cell >> 5] |= (cell & 0x1f);
	o->gray = 0;

	/* walk pointers from this object, and
	 * push them onto their arena's gs. */
}

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

int main(void) {
	printf( "sizes: arena meta: %zd a: %zd b: %zd: gs: %zd\n",
			sizeof(struct arena),
			sizeof(struct arena_meta_a),
			sizeof(struct arena_meta_b),
			sizeof(struct gs));
	return 0;
}
