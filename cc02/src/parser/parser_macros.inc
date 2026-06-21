#ifndef __PARSER_MACROS_INC__
#define __PARSER_MACROS_INC__

#include <stdlib.h>


/*
 * Error generator macros
 * -----------------------
 * GENERATE_ERROR(TYPE, TOKEN, EXPECTED, CTX)
 *   Sets up an error structure in the parser, storing the error type, the
 *   token that was found, what was expected, and the context where the
 *   error occurred. Sets p->has_errored flag to prevent further parsing.
 *   TYPE is one of the ERR_* parser kinds from errors.h (ERR_UNEXPECTED_EOF,
 *   ERR_UNEXPECTED_TOKEN, ERR_ALLOCATION_FAILED) - e->loc is taken from
 *   TOKEN's own loc, since for parser errors the token IS the location.
 *
 * GENERATE_ALLOCATOR_ERROR()
 *   Generates an ERR_ALLOCATION_FAILED error when malloc/realloc fails.
*/
#define GENERATE_ERROR(TYPE, TOKEN, EXPECTED, CTX)                          \
  do {                                                                      \
    p->err = malloc(sizeof(error_t));                                       \
    if (!p->err) {                                                          \
      fprintf(stderr, "fatal: out of memory while reporting a parse error\n"); \
      exit(1);                                                              \
    }                                                                       \
    p->err->type = TYPE;                                                    \
    p->err->loc = (TOKEN).loc;                                              \
    p->err->parse.found = TOKEN;                                            \
    p->err->parse.expected = EXPECTED;                                      \
    p->err->parse.context = CTX;                                            \
    p->has_errored = 1;                                                     \
  } while(0)                                                                \


#define GENERATE_ALLOCATOR_ERROR()                                          \
  do {                                                                      \
    token_t err = {                                                         \
      .type = t_eof,                                                        \
      .loc = {                                                              \
        .line = __LINE__,                                                   \
        .column = 0,                                                        \
        .length = 0,                                                        \
        .file_path = "parser.c" } };                                        \
    GENERATE_ERROR(ERR_ALLOCATION_FAILED, err, "", "");                     \
  } while(0);


/*
 * Scratch buffer macros
 * ----------------------
 * Both parser.c's growable scratch buffers (used while accumulating a
 * variable-length list before committing it into the arena as a fixed-size
 * array) follow the same realloc-doubling pattern. There are two shapes:
 *
 *   DEFINE_NODE_SCRATCH(NAME)
 *     For buffers of node_t* (arena-allocated AST nodes referenced by
 *     pointer). Use this for things like statement lists, call args, etc.
 *     Expands to a NAME##_t type storing node_t** items, matching the
 *     existing node_list_t shape.
 *
 *   DEFINE_VALUE_SCRATCH(NAME, ELEM_TYPE, LIST_TYPE)
 *     For buffers of plain value structs (param_t, field_t, field_init_t,
 *     etc. — copied by value, not referenced by pointer). LIST_TYPE is
 *     passed explicitly rather than derived via token-pasting, since
 *     ELEM_TYPE##_list_t can't be built automatically when ELEM_TYPE
 *     itself might contain a '*' (as in the node case) — passing it
 *     explicitly also means the naming convention isn't forced on you.
 *
 * Both expand to: a NAME##_t struct, a static NAME##_push(), and a
 * static NAME##_commit().
 */ 
#define DEFINE_NODE_SCRATCH(NAME)                                           \
typedef struct {                                                            \
  node_t **items;                                                           \
  unsigned count;                                                           \
  unsigned capacity;                                                        \
} NAME##_t;                                                                 \
                                                                            \
static void NAME##_push(NAME##_t *s, node_t *val, parser_t *p) {            \
  if (s->count == s->capacity) {                                            \
    s->capacity = s->capacity ? s->capacity * 2 : 8;                        \
    node_t **grown = realloc(s->items, sizeof(node_t*) * s->capacity);      \
    if (!grown) {                                                           \
      GENERATE_ALLOCATOR_ERROR();                                           \
      return;                                                               \
    }                                                                       \
    s->items = grown;                                                       \
  }                                                                         \
  s->items[s->count++] = val;                                               \
}                                                                           \
                                                                            \
static node_list_t NAME##_commit(NAME##_t *s, arena_t *arena) {             \
  node_list_t list = { NULL, 0 };                                           \
  if (s->count > 0) {                                                       \
    node_t **items = arena_alloc(arena, sizeof(node_t*) * s->count);        \
    memcpy(items, s->items, sizeof(node_t*) * s->count);                    \
    list.items = items;                                                     \
    list.count = s->count;                                                  \
  }                                                                         \
  s->count = 0;                                                             \
  return list;                                                              \
}
 
 
#define DEFINE_VALUE_SCRATCH(NAME, ELEM_TYPE, LIST_TYPE)                    \
typedef struct {                                                            \
  ELEM_TYPE *items;                                                         \
  unsigned   count;                                                         \
  unsigned   capacity;                                                      \
} NAME##_t;                                                                 \
                                                                            \
static void NAME##_push(NAME##_t *s, ELEM_TYPE val, parser_t *p) {          \
  if (s->count == s->capacity) {                                            \
    s->capacity = s->capacity ? s->capacity * 2 : 8;                        \
    ELEM_TYPE *grown = realloc(s->items, sizeof(ELEM_TYPE) * s->capacity);  \
    if (!grown) {                                                           \
      GENERATE_ALLOCATOR_ERROR();                                           \
      return;                                                               \
    }                                                                       \
    s->items = grown;                                                       \
  }                                                                         \
  s->items[s->count++] = val;                                               \
}                                                                           \
                                                                            \
static LIST_TYPE NAME##_commit(NAME##_t *s, arena_t *arena) {               \
  LIST_TYPE list = { NULL, 0 };                                             \
  if (s->count > 0) {                                                       \
    ELEM_TYPE *items = arena_alloc(arena, sizeof(ELEM_TYPE) * s->count);    \
    memcpy(items, s->items, sizeof(ELEM_TYPE) * s->count);                  \
    list.items = items;                                                     \
    list.count = s->count;                                                  \
  }                                                                         \
  s->count = 0;                                                             \
  return list;                                                              \
}


/*
 * Token expectation macro
 * ------------------------
 * Verifies that the current token matches the expected type. If it doesn't,
 * generates an error with the provided expected description and context.
 */
#define EXPECT_SYMBOL(EXPECTED, ERR_EXPECTED, ERR_CTX)                      \
  do {                                                                      \
    if (CUR_TOK.type != EXPECTED) {                                         \
      GENERATE_ERROR(ERR_UNEXPECTED_TOKEN, CUR_TOK, ERR_EXPECTED, ERR_CTX); \
    }                                                                       \
  } while(0);


/*
 * Recursive descent macro
 * ------------------------
 * Implements a binary operator parsing level for operator precedence climbing.
 * Takes a NAME for the function, NEXT for the next precedence level, and
 * __VA_ARGS__ for case statements matching operator token types. Parses left-
 * associative binary expressions and builds an AST with NODE_BINOP nodes.
 */
#define BINOP_LEVEL(NAME, NEXT, ...)                                        \
static node_t *NAME(parser_t *p) {                                          \
  node_t *left = NEXT(p);                                                   \
  GUARD(p);                                                                 \
  for (;;) {                                                                \
    op_t op;                                                                \
    switch (CUR_TOK.type) {                                                 \
      __VA_ARGS__                                                           \
      default: return left;                                                 \
    }                                                                       \
                                                                            \
    ++p->pos; /* consume operator */                                        \
                                                                            \
    node_t *right = NEXT(p);                                                \
    GUARD(p);                                                               \
                                                                            \
    node_t *n = ALLOC_NODE(p);                                              \
    n->kind = NODE_BINOP;                                                   \
    n->binop.left  = left;                                                  \
    n->binop.op    = op;                                                    \
    n->binop.right = right;                                                 \
                                                                            \
    left = n;                                                               \
  }                                                                         \
}


#endif