#include "arena.h"

#include <stdlib.h>
#include <string.h>


int arena_init(arena_t *a, size_t chunk_size) {
  a->chunk_size = chunk_size;
  a->first = malloc(sizeof(arena_chunk_t) + chunk_size);
  if (!a->first) {
    return 0;
  }

  a->first->next = NULL;
  a->first->used = 0;
  a->first->capacity = chunk_size;
  a->current = a->first;

  return 1;
}


void *arena_alloc(arena_t *a, size_t size) {
  size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);

  if (a->current->used + size > a->current->capacity) {
    if (size > a->chunk_size) {
      // Oversized allocation: insert after current, don't advance current
      arena_chunk_t *chunk = malloc(sizeof(arena_chunk_t) + size);
      if (!chunk) exit(1);
      chunk->used = size;
      chunk->capacity = size;
      chunk->next = a->current->next;
      a->current->next = chunk;
      memset(chunk->data, 0, size);
      return chunk->data;
    } else {
      // Standard allocation: create a new chunk and advance current.
      // Splice it in *after* current rather than assuming current is the
      // tail - an earlier oversized allocation may have inserted a chunk
      // after current without advancing it, and overwriting current->next
      // here would orphan that chunk (and leak it past arena_free).
      arena_chunk_t *chunk = malloc(sizeof(arena_chunk_t) + a->chunk_size);
      if (!chunk) exit(1);
      chunk->used = size;
      chunk->capacity = a->chunk_size;
      chunk->next = a->current->next;

      a->current->next = chunk;
      a->current = chunk;
      memset(chunk->data, 0, size);
      return chunk->data;
    }
  }

  void *ptr = a->current->data + a->current->used;
  a->current->used += size;
  memset(ptr, 0, size);
  return ptr;
}


void arena_free(arena_t *a) {
  if (!a) return;

  arena_chunk_t *curr = a->first;
  while (curr) {
    arena_chunk_t *tmp = curr;
    curr = curr->next;
    free(tmp);
  }

  a->chunk_size = 0;
  a->current = a->first = NULL;
}