#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "arena.h"

int main(void) {
  // --- basic alloc and multi-chunk growth ---
  {
    arena_t arena;
    assert(arena_init(&arena, 64));

    int *a = arena_alloc(&arena, sizeof(int));
    assert(a != NULL);
    assert(*a == 0);
    *a = 42;
    assert(*a == 42);

    int *b = arena_alloc(&arena, sizeof(int));
    int *c = arena_alloc(&arena, sizeof(int));
    assert(b != NULL && c != NULL);
    assert(b != a && c != b && c != a);
    *b = 100;
    *c = 200;
    assert(*a == 42);
    assert(*b == 100);
    assert(*c == 200);

    // many small allocations to force multiple chunk growths
    for (int i = 0; i < 500; i++) {
      int *p = arena_alloc(&arena, sizeof(int));
      assert(p != NULL);
      *p = i;
    }
    assert(*a == 42);

    arena_free(&arena);
  }

  // --- oversized allocations interleaved with standard chunk growth ---
  // Regression test: an oversized alloc splices a chunk in after `current`
  // without advancing it. A subsequent standard new-chunk alloc must keep
  // that spliced chunk linked (it used to overwrite current->next and
  // orphan the oversized chunk, leaking it past arena_free). Everything
  // here lives in one arena so valgrind catches a regression.
  {
    arena_t arena;
    assert(arena_init(&arena, 64));

    char *small = arena_alloc(&arena, 32);   // fits in first chunk (used=32)
    assert(small != NULL);
    memset(small, 'A', 32);

    char *big = arena_alloc(&arena, 200);    // oversized -> spliced after first
    assert(big != NULL);
    memset(big, 'X', 200);

    char *more = arena_alloc(&arena, 40);    // 32+40 > 64 -> standard new chunk
    assert(more != NULL);                    // must not orphan the 200-byte chunk
    memset(more, 'B', 40);

    char *huge = arena_alloc(&arena, 1024);  // another oversized splice
    assert(huge != NULL);
    memset(huge, 'Y', 1024);

    // every prior allocation is still intact and independent
    assert(small[0] == 'A' && small[31] == 'A');
    assert(big[0] == 'X' && big[199] == 'X');
    assert(more[0] == 'B' && more[39] == 'B');
    assert(huge[0] == 'Y' && huge[1023] == 'Y');

    arena_free(&arena);
  }

  // --- ARENA_ALLOC macro ---
  {
    arena_t arena;
    assert(arena_init(&arena, 256));

    typedef struct { int x; int y; } point_t;
    point_t *pt = ARENA_ALLOC(&arena, point_t);
    assert(pt != NULL);
    assert(pt->x == 0 && pt->y == 0);
    pt->x = 10;
    pt->y = 20;
    assert(pt->x == 10 && pt->y == 20);

    arena_free(&arena);
  }

  // --- edge cases ---
  {
    arena_t arena;
    assert(arena_init(&arena, 64));

    void *z = arena_alloc(&arena, 0);
    assert(z != NULL);

    arena_free(&arena);

    // double free should be safe
    arena_free(&arena);
  }

  printf("all arena smoke tests passed\n");
  return 0;
}
