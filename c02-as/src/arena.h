#ifndef __ARENA_H__
#define __ARENA_H__

/*
 * arena.h - generic chunked bump allocator
 * -----------------------------------------
 * A simple growable arena: allocations are bump-pointer within the current
 * chunk, and a new chunk is malloc'd (and linked in) once the current one
 * fills up. Nothing is ever freed individually — the whole arena is torn
 * down at once with arena_free(). This is the right shape for things that
 * are all allocated together and all die together (an AST, a symbol table,
 * a hash map's entries, etc).
 *
 * USAGE
 * -----
 *   arena_t a;
 *   arena_init(&a, MY_CHUNK_SIZE);
 *   my_struct_t *s = ARENA_ALLOC(&a, my_struct_t);   // zeroed, cast for you
 *   void *raw = arena_alloc(&a, some_size);          // zeroed, untyped
 *   ...
 *   arena_free(&a);
 *
 * Any single allocation larger than the chunk size is still served (as its
 * own oversized chunk, spliced into the chain after the current chunk) -
 * arena_alloc never fails to satisfy a request for lack of chunk size,
 * it only fails (and exit(1)s) if malloc itself fails.
 */

#include <stddef.h>

typedef struct arena_chunk_t {
  struct arena_chunk_t *next;
  size_t used;
  size_t capacity;
  char data[];  // flexible array member
} arena_chunk_t;

typedef struct {
  arena_chunk_t *first;
  arena_chunk_t *current;
  size_t chunk_size;
} arena_t;


// Initialises `a` with a first chunk of `chunk_size` bytes.
// Returns 1 on success, 0 if the initial malloc failed.
int arena_init(arena_t *a, size_t chunk_size);

// Bump-allocates `size` bytes (zeroed), growing the arena with a new chunk
// if needed. Never returns NULL - exits the process if malloc fails.
void *arena_alloc(arena_t *a, size_t size);

// Frees every chunk in the arena. Safe to call on a zeroed/already-freed arena.
void arena_free(arena_t *a);


// Allocates and zeroes sizeof(TYPE) from `a`, cast to TYPE*. This is the
// macro you want at most call sites - arena_alloc() is the raw version for
// when you need a dynamic/non-type size (e.g. a flexible array, a list of
// N items).
#define ARENA_ALLOC(a, TYPE) ((TYPE *)arena_alloc((a), sizeof(TYPE)))

#endif // __ARENA_H__