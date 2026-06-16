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


#define GENERATE_ERROR(PARSER, TYPE, TOKEN, EXPECTED, CTX) \
  {PARSER->err = malloc(sizeof(error_t));                  \
  PARSER->err->type = TYPE;                                \
  PARSER->err->found = TOKEN;                              \
  PARSER->err->expected = EXPECTED;                        \
  PARSER->err->context = CTX;                              \
  PARSER->has_errored = 1;}


#define EXPECT_SYMBOL(PARSER, TOKEN, EXPECTED, ERR_EXPECTED, ERR_CTX)         \
  do {                                                                        \
    if (TOKEN.type != EXPECTED) {                                             \
      GENERATE_ERROR(PARSER, UNEXPECTED_TOKEN, TOKEN, ERR_EXPECTED, ERR_CTX); \
    }                                                                         \
  } while(0);


// dirty panic, debug only, memory leak central
#define UNIMPLEMENTED_PANIC()                                        \
    fprintf(stderr, "Unimplemented at %s:%d\n", __FILE__, __LINE__); \
    exit(1);


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


static inline int token_type_to_parser_type(token_type_t t) {
  switch (t) {
    case t_u8: return TYPE_U8;
    case t_i8: return TYPE_I8;
    case t_u16: return TYPE_U16;
    case t_i16: return TYPE_I16;
    case Kw_void: return TYPE_VOID;
    default: return -1;
  }
}


// ----------------------------------------------------------------
// Recursive descent parsing functions
// ----------------------------------------------------------------

// shorthand to check if parser threw an error
#define GUARD(p) do { if ((p)->err) return NULL; } while(0)
#define CUR_TOK p->tokens[p->pos]


static node_t *logical_or(parser_t *p); // recursive descent entry point


static inline int is_token_type_name(parser_t *p) {
  switch (CUR_TOK.type) {
    case t_u8: case t_i8: case t_u16:  case t_i16: case Kw_void:
      return 1;
    default:
      return 0;
  }
}


static type_t parse_type(parser_t *p) {
  type_t res = { 0 };

  int type;
  if ((type = token_type_to_parser_type(CUR_TOK.type)) == -1)
    GENERATE_ERROR(p, UNEXPECTED_TOKEN, CUR_TOK, "Type name", "type parsing");

  res.kind = (type_kind_t)type;

  ++p->pos; // consume type

  // check for pointer
  while (CUR_TOK.type == s_star) {
    res.is_ptr = 1;
    ++res.ptr_depth;
    ++p->pos; // consume star
  }

  return res;
}


/* consumes token, returning next one -> throws error if EOF is encountered */
static token_t consume(parser_t *p) {
  if (p->pos >= p->count || CUR_TOK.type == t_eof) {
    GENERATE_ERROR(p, UNEXPECTED_EOF, CUR_TOK, "token", "expression");
    return CUR_TOK; // return whatever's there, caller checks p->err
  }

  return p->tokens[p->pos++];
}


static node_t *primary(parser_t *p) {
  token_t tok = consume(p);
  GUARD(p);

  switch (tok.type) {
    case l_num: { // literal
      node_t *node = ALLOC_NODE(p);
      node->kind = NODE_NUMBER;
      node->number = *(long*)tok.value;
      return node;
    }

    case l_string: {
      node_t *node = ALLOC_NODE(p);
      node->kind = NODE_STRING;
      node->value = (char*)tok.value;
      return node;
    }

    case s_lparen: { // cast or grouped expression
      unsigned is_cast = is_token_type_name(p);

      if (is_cast) {
        type_t t = parse_type(p);
        GUARD(p);

        EXPECT_SYMBOL(p, CUR_TOK, s_rparen, ")", "cast expression");
        GUARD(p);
        ++p->pos; // consume )

        node_t *rhs = logical_or(p);
        GUARD(p);

        node_t *cast = ALLOC_NODE(p);
        cast->kind = NODE_CAST;
        cast->cast.cast_type = t;
        cast->cast.operand = rhs;

        return cast;
      } else {
        node_t *expr = logical_or(p);

        EXPECT_SYMBOL(p, CUR_TOK, s_rparen, ")", "grouped expression");
        GUARD(p);
        ++p->pos; // consume )

        return expr;
      }
    }

    case l_identifier: {
      char *name = (char*)tok.value;

      if (CUR_TOK.type == s_lparen) {
        ++p->pos; // consume (
        scratch_t args = {0};
      
        while (CUR_TOK.type != s_rparen) {
          scratch_push(&args, logical_or(p));
          if (p->err) { free(args.items); return NULL; }
          if (CUR_TOK.type == s_comma) ++p->pos;
        }
      
        ++p->pos; // consume )
        node_t *call = ALLOC_NODE(p);
        call->kind = NODE_CALL;
        call->call.name = name;
        call->call.args = scratch_commit(&args, p->arena);
      
        free(args.items);
        return call;
      } else {
        node_t *ident = ALLOC_NODE(p);
        ident->kind = NODE_IDENTIFIER;
        ident->identifier = name;
        return ident;
      }
    }

    default:
      GENERATE_ERROR(p, UNEXPECTED_TOKEN, tok, "expression", "primary");
      return NULL;
  }
}


static node_t *unary(parser_t *p) {
  switch (CUR_TOK.type) {
    case s_bang: {
      ++p->pos;

      node_t *operand = unary(p);
      GUARD(p);

      node_t *n = ALLOC_NODE(p);
      n->kind = NODE_UNARY;
      n->unary.op = OP_BANG;
      n->unary.operand = operand;

      return n;
    }

    case s_minus: {
      ++p->pos;

      node_t *operand = unary(p);
      GUARD(p);

      node_t *n = ALLOC_NODE(p);
      n->kind = NODE_UNARY;
      n->unary.op = OP_NEGATE;
      n->unary.operand = operand;

      return n;
    }

    case s_ampersand: {
      ++p->pos;

      node_t *operand = unary(p);
      GUARD(p);

      node_t *n = ALLOC_NODE(p);
      n->kind = NODE_UNARY;
      n->unary.op = OP_ADDRESSOF;
      n->unary.operand = operand;

      return n;
    }

    case s_star: case s_mem_lookup: {
      ++p->pos;

      node_t *operand = unary(p);
      GUARD(p);

      node_t *n = ALLOC_NODE(p);
      n->kind = NODE_DEREF;
      n->deref_target = operand;

      return n;
    }

    default: return primary(p);
  }
}


static node_t *factor(parser_t *p) {
  node_t *left = unary(p);
  GUARD(p);

  // collect terms in a compount multiplication / division
  for (;;) {
    op_t op;
    switch (CUR_TOK.type) {
      case s_star:   op = OP_MULTIPLY; break;
      case s_divide: op = OP_DIVIDE;   break;
      default: return left;
    }
 
    ++p->pos; // consume operator
 
    node_t *right = unary(p);
    GUARD(p);
 
    node_t *n = ALLOC_NODE(p);
    n->kind = NODE_BINOP;
    n->binop.left  = left;
    n->binop.op    = op;
    n->binop.right = right;
  
    left = n; // setup next iteration
  }
}


static node_t *term(parser_t *p) {
  node_t *left = factor(p);
  GUARD(p);

  // collect terms in a compount summation / addition
  for (;;) {
    op_t op;
    switch (CUR_TOK.type) {
      case s_plus:   op = OP_PLUS; break;
      case s_minus:  op = OP_MINUS;   break;
      default: return left;
    }
 
    ++p->pos; // consume operator
 
    node_t *right = factor(p);
    GUARD(p);
 
    node_t *n = ALLOC_NODE(p);
    n->kind = NODE_BINOP;
    n->binop.left  = left;
    n->binop.op    = op;
    n->binop.right = right;
  
    left = n; // setup next iteration
  }
}


static node_t *comparison(parser_t *p) {
  node_t *left = term(p);
  GUARD(p);

  for (;;) {
    op_t op;
    switch (CUR_TOK.type) {
      case s_lt:   op = OP_LT; break;
      case s_lte:  op = OP_LTE; break;
      case s_gt:   op = OP_GT; break;
      case s_gte:  op = OP_GTE; break;
      default: return left;
    }
 
    ++p->pos; // consume operator
 
    node_t *right = term(p);
    GUARD(p);
 
    node_t *n = ALLOC_NODE(p);
    n->kind = NODE_BINOP;
    n->binop.left  = left;
    n->binop.op    = op;
    n->binop.right = right;
  
    left = n; // setup next iteration
  }
}


static node_t *equality(parser_t *p) {
  node_t *left = comparison(p);
  GUARD(p);

  for (;;) {
    op_t op;
    switch (CUR_TOK.type) {
      case s_equalsequals:  op = OP_EQUALSEQUALS; break;
      case s_bang_equals:   op = OP_BANGEQUALS;   break;
      default: return left;
    }
 
    ++p->pos; // consume operator
 
    node_t *right = comparison(p);
    GUARD(p);
 
    node_t *n = ALLOC_NODE(p);
    n->kind = NODE_BINOP;
    n->binop.left  = left;
    n->binop.op    = op;
    n->binop.right = right;
  
    left = n; // setup next iteration
  }
}


static node_t *logical_and(parser_t *p) {
  node_t *left = equality(p);
  GUARD(p);
  for (;;) {
    switch (CUR_TOK.type) {
      case s_and: {
        ++p->pos;
        node_t *right = equality(p);
        GUARD(p);
 
        node_t *n = ALLOC_NODE(p);
        n->kind = NODE_BINOP;
        n->binop.left  = left;
        n->binop.op    = OP_AND;
        n->binop.right = right;
 
        left = n;
        break;
      }
 
      default: return left;
    }
  }
}


// root of recursive descent
// logical_or   looks for ||         calls logical_and
// logical_and  looks for &&         calls equality
// equality     looks for == !=      calls comparison
// comparison   looks for < > <= >=  calls term
// term         looks for + -        calls factor
// factor       looks for * /        calls unary
// unary        looks for * @ ! -    calls primary
// primary      looks for literals, identifiers, (expr)   consumes tokens
static node_t *logical_or(parser_t *p) {
  node_t *left = logical_and(p);
  GUARD(p);
  for (;;) {
    switch (CUR_TOK.type) {
      case s_or: {
        ++p->pos;
        node_t *right = logical_and(p);
        GUARD(p);
 
        node_t *n = ALLOC_NODE(p);
        n->kind = NODE_BINOP;
        n->binop.left  = left;
        n->binop.op    = OP_OR;
        n->binop.right = right;
 
        left = n;
        break;
      }
 
      default: return left;
    }
  }
}


// wrapper to make code more readable in higher level parsing functions
static node_t *parse_expr(parser_t *p) {
  return logical_or(p);
}


// ----------------------------------------------------------------
// High level parsing
// ----------------------------------------------------------------

static node_t *parse_reg_decl(parser_t *p) {
  ++p->pos; // consume reg keyword

  node_t *decl = ALLOC_NODE(p);
  decl->kind = NODE_REG_DECL;

  type_t type = parse_type(p);  // consumes type token
  GUARD(p);

  decl->reg_decl.type = type;

  EXPECT_SYMBOL(p, CUR_TOK, l_identifier, "Identifier", "register decl")
  GUARD(p);

  // pull identifier from token array
  decl->reg_decl.name = (char*)CUR_TOK.value;
  ++p->pos; // consume identifier

  EXPECT_SYMBOL(p, CUR_TOK, s_mem_lookup, "@", "register decl")
  GUARD(p);
  ++p->pos; // consume @

  EXPECT_SYMBOL(p, CUR_TOK, l_num, "Memmry address literal", "register decl")
  GUARD(p);

  // pull integer literal (dereference void* to get the stored unsigned long)
  decl->reg_decl.addr = *(unsigned long*)CUR_TOK.value;
  ++p->pos; // consume integer literal

  EXPECT_SYMBOL(p, CUR_TOK, s_semicolon, ";", "register decl")
  GUARD(p);
  ++p->pos; // consume ;

  return decl;
}


static node_t *parse_global_var_decl(parser_t *p) {
  node_t *decl = ALLOC_NODE(p);
  decl->kind = NODE_GLOBAL_VAR;

  type_t type = parse_type(p); // consumes type token
  GUARD(p);

  decl->global_var.type = type;

  EXPECT_SYMBOL(p, CUR_TOK, l_identifier, "Identifier", "register decl")
  GUARD(p);

  // pull identifier from token array
  decl->reg_decl.name = (char*)CUR_TOK.value;
  ++p->pos; // consume identifier

  if (CUR_TOK.type == s_equals) { // has initialiser, parse it
    ++p->pos; // consume =
    decl->global_var.initialiser = parse_expr(p);
    GUARD(p);
  }

  EXPECT_SYMBOL(p, CUR_TOK, s_semicolon, "; or initialiser expresion", "global var decl")
  GUARD(p);
  ++p->pos; // consume ;

  return decl;
}


static node_t *parse_toplevel(parser_t *p) {
  token_t tok = CUR_TOK; 

  switch (tok.type) {
    case Kw_fn:
      ++p->pos;
      UNIMPLEMENTED_PANIC()
    case Kw_reg:
      return parse_reg_decl(p);
    case t_u8: case t_i8: case t_u16: case t_i16: case Kw_void:
      return parse_global_var_decl(p);

    default:
      GENERATE_ERROR(p, UNEXPECTED_TOKEN, tok, "Top level declaration. (Variable/Register decl, Function decl)", "top level parse");
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
