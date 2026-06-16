#include "parser.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef enum {
  UNEXPECTED_EOF,
  UNEXPECTED_TOKEN
} error_type_t;


typedef struct {
  error_type_t type;

  token_t found;
  char *expected;

  char *context;
} error_t;


typedef struct {
  parser_arena_t *arena;

  token_t *tokens;
  unsigned count;
  unsigned pos;
  
  unsigned has_errored;
  error_t *err;
} parser_t;


typedef struct {
  node_t **items;
  unsigned count;
  unsigned capacity;
} scratch_t;


static void print_parse_error(error_t *e) {
  switch (e->type) {
    case UNEXPECTED_EOF:
      fprintf(stderr, "%s:%u:%u: Unexpected end of file while %s: expected %s.\n", 
               e->found.file_path, e->found.line, e->found.column, 
               e->context, e->expected);
      return;
    case UNEXPECTED_TOKEN:
      if (token_has_value(e->found.type)) {
        unsigned should_free = 0;
        char *val = token_val_to_string(e->found, &should_free);

        fprintf(stderr,"%s:%u:%u: Unexpected token while %s:\n\texpected %s, got %s: %s.\n", 
                 e->found.file_path, e->found.line, e->found.column, 
                 e->context, e->expected, token_type_to_string(e->found.type), val);
        
        // if the token is a number literal, a buffer will be allocated that needs to freed.
        // I could just hard check token type == number literal but it feels sloppy to make 
        // the caller have to track which token vals need freeing when getting them as strings.
        if (should_free) free(val);
      } else {
        fprintf(stderr,"%s:%u:%u: Unexpected token while %s:\n\texpected %s, got %s.\n", 
                 e->found.file_path, e->found.line, e->found.column, 
                 e->context, e->expected, token_type_to_string(e->found.type));
      }

      return;
  }
}


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
  size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);

  if (a->current->used + size > a->current->capacity) {
    // if size exceeds standard chunk size, allocate a dedicated chunk
    // just for this allocation, otherwise allocate a standard chunk
    size_t alloc_size = size > a->chunk_size ? size : a->chunk_size;

    arena_chunk_t *chunk = malloc(sizeof(arena_chunk_t) + alloc_size);
    if (!chunk) {
      fprintf(stderr, "fatal: arena allocation failed\n");
      exit(1);
    }

    chunk->next = NULL;
    chunk->used = size;  // immediately consume the space we need
    chunk->capacity = alloc_size;

    // insert BEFORE current so current can still serve future small allocs
    // current: [/////|       ]   <-- still has room
    // new:     [XXXXX]           <-- oversized, dedicated
    //
    // without this, current gets replaced and its remaining space is wasted
    chunk->next = a->current->next;
    a->current->next = chunk;

    return chunk->data;
  }

  void *ptr = a->current->data + a->current->used;
  a->current->used += size;
  return ptr;
}


#define ALLOC_NODE(p) (memset(parser_alloc((p)->arena, sizeof(node_t)), 0, sizeof(node_t)))


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


static void scratch_push(scratch_t *s, node_t *node) {
  if (s->count == s->capacity) {
    s->capacity = s->capacity ? s->capacity * 2 : 8;
    node_t **grown = realloc(s->items, sizeof(node_t*) * s->capacity);
    if (!grown) { /* handle */ return; }
    s->items = grown;
  }
  s->items[s->count++] = node;
}


static node_list_t scratch_commit(scratch_t *s, parser_arena_t *arena) {
  node_list_t list = { NULL, 0 };
  if (s->count > 0) {
    node_t **items = parser_alloc(arena, sizeof(node_t*) * s->count);
    memcpy(items, s->items, sizeof(node_t*) * s->count);
    list.items = items;
    list.count = s->count;
  }
  s->count = 0;
  return list;
}


static node_t *parse_toplevel(parser_t *p) {
  token_t tok = p->tokens[p->pos]; 

  switch (tok.type) {
    case Kw_fn:
      ++p->pos;
      return NULL;
    case Kw_reg:
      ++p->pos;
      return NULL;
    case t_u8: case t_i8: case t_u16: case t_i16:
      ++p->pos;
      return NULL;
    case t_eof: 
      return NULL;
      break;

    default:
      p->err = malloc(sizeof(error_t));
      p->err->type = UNEXPECTED_TOKEN;
      p->err->found = tok;
      p->err->expected = "Top level declaration. (Variable/Register decl, Function decl)";
      p->err->context = "top level parse";
      
      p->has_errored = 1;
      return NULL;
  }
}


node_t *parse(token_t *tokens, unsigned num_tokens, parser_arena_t *mem_area) {
  parser_t p = { mem_area, tokens, num_tokens, 0, 0, NULL };

  scratch_t scratch = {0};
  while (p.pos < p.count && p.tokens[p.pos].type != t_eof) {
    scratch_push(&scratch, parse_toplevel(&p));

    if (p.has_errored) {
      print_parse_error(p.err);

      free(scratch.items);
      free(p.err);
      p.err = NULL;
      return NULL;
    }
  }

  node_t *root = ALLOC_NODE(&p);
  root->kind = NODE_PROGRAM;
  root->program = scratch_commit(&scratch, p.arena);

  free(scratch.items);
  return root;
}
