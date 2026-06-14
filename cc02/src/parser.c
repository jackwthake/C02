#include "parser.h"

#include <stdlib.h>
#include <stdio.h>


typedef struct {
  token_t *tokens;
  unsigned count;
  unsigned pos;
  parser_arena_t *arena;
} parser_t;


int parser_init(parser_arena_t *a, size_t chunk_size) {
  chunk_size = (chunk_size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);

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


static void *parser_alloc(parser_arena_t *a, size_t size) {
  // ensure size is even
  size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);

  if (a->current->used + size > a->current->capacity) {
    arena_chunk_t *chunk = malloc(sizeof(arena_chunk_t) + a->chunk_size);
    chunk->next = NULL;
    chunk->used = 0;
    chunk->capacity = a->chunk_size;
    a->current->next = chunk;
    a->current = chunk;
  }
 
  void *ptr = a->current->data + a->current->used;
  a->current->used += size;
 
  return ptr;
}


void parser_free(parser_arena_t *a) {
  if (a) {
    arena_chunk_t *curr = a->first;
    while (curr) {
      arena_chunk_t *tmp = curr;
      curr = curr->next;
      free(tmp);
    }

    a->chunk_size = 0;
    a->current = a->first = NULL;
  }
}


node_t *parse(token_t *tokens, unsigned num_tokens, parser_arena_t *mem_area) {
  parser_t p = { tokens, num_tokens, 0, mem_area };
  return NULL;
}