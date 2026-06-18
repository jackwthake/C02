/*
 * parser.c - C02 recursive descent parser
 *
 * OVERVIEW
 * --------
 * Converts a flat token array (from tokenizer.c) into an AST rooted at a
 * NODE_PROGRAM node. All AST nodes are allocated from a chunked arena
 * (parser_arena_t) — call parser_init() before parse() and parser_free()
 * when done. String fields in nodes (names, identifiers) point directly into
 * the token array; the token array must outlive the AST.
 *
 * RECURSIVE DESCENT CHAIN
 * -----------------------
 * parse_expr() is the public entry point for expression parsing. It delegates
 * down a precedence chain (lowest to highest):
 *
 *   logical_or -> logical_and -> equality -> comparison
 *              -> term -> factor -> unary -> primary
 *
 * primary() handles literals (number, string), identifiers, function calls,
 * grouped expressions (expr), and C-style casts (type)expr.
 * unary() handles prefix operators: !, -, &, * / @  (deref).
 *
 * STATEMENT PARSING
 * -----------------
 * parse_stmt() dispatches on the current token:
 *   Kw_return    -> NODE_RETURN
 *   Kw_while     -> NODE_WHILE   (cond + block)
 *   Kw_for       -> NODE_FOR     (init + cond + incr + block)
 *   Kw_if        -> NODE_IF      (cond + then block + optional else block)
 *   type keyword -> NODE_VAR_DECL
 *   s_star       -> NODE_DEREF_ASSIGN
 *   l_identifier -> NODE_ASSIGN (peek: next == s_equals)
 *                -> NODE_CALL   (peek: next == s_lparen)
 *
 * For loops use parse_for_initializer_clause() for the initialiser slot, which handles
 * both type-led declarations and plain expressions without consuming a
 * trailing semicolon (parse_stmt() would eat it). The cond and incrementer
 * slots use parse_expr() directly.
 *
 * parse_block() accumulates statements into a scratch buffer and commits them
 * to the arena as a node_list_t. parse_function_params() does the same for
 * param_t using a parallel param_scratch_t.
 *
 * TOP-LEVEL PARSING
 * -----------------
 * parse_toplevel() dispatches on:
 *   Kw_fn        -> parse_function()   (name + params + return type + block)
 *   Kw_reg       -> parse_reg_decl()   (type + name + @ + address)
 *   type keyword -> parse_global_var_decl()
 *
 * ERROR HANDLING
 * --------------
 * Errors are stored in parser_t.err (heap allocated error_t). Once set, all
 * recursive descent functions propagate NULL upward via GUARD(p). The top
 * level parse() loop catches the error, prints it via print_parse_error(),
 * frees the error and returns NULL. Only the first error is reported.
 * EXPECT_SYMBOL sets the error but does NOT return — callers must GUARD(p)
 * immediately after every EXPECT_SYMBOL call.
 */

#include "parser.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  node_t **items;
  unsigned count;
  unsigned capacity;
} scratch_t;


typedef struct {
  param_t  *items;
  unsigned  count;
  unsigned  capacity;
} param_scratch_t;


typedef struct {
  parser_arena_t *arena;

  token_t *tokens;
  unsigned count;
  unsigned pos;
  
  unsigned has_errored;
  error_t *err;
} parser_t;


#define GENERATE_ERROR(TYPE, TOKEN, EXPECTED, CTX) \
  do {                                             \
    p->err = malloc(sizeof(error_t));              \
    p->err->type = TYPE;                           \
    p->err->found = TOKEN;                         \
    p->err->expected = EXPECTED;                   \
    p->err->context = CTX;                         \
    p->has_errored = 1;                            \
  } while(0)                                       \


#define EXPECT_SYMBOL(EXPECTED, ERR_EXPECTED, ERR_CTX)                     \
  do {                                                                     \
    if (CUR_TOK.type != EXPECTED) {                                        \
      GENERATE_ERROR(UNEXPECTED_TOKEN, CUR_TOK, ERR_EXPECTED, ERR_CTX);    \
    }                                                                      \
  } while(0);


// dirty panic, debug only, memory leak central
#define UNIMPLEMENTED_PANIC()                                        \
    fprintf(stderr, "Unimplemented at %s:%d\n", __FILE__, __LINE__); \
    exit(1);


extern void print_parse_error(error_t *e);


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


static void param_scratch_push(param_scratch_t *s, param_t param) {
  if (s->count == s->capacity) {
    s->capacity = s->capacity ? s->capacity * 2 : 8;
    param_t *grown = realloc(s->items, sizeof(param_t) * s->capacity);
    if (!grown) return;
    s->items = grown;
  }
  s->items[s->count++] = param;
}


static param_list_t param_scratch_commit(param_scratch_t *s, parser_arena_t *arena) {
  param_list_t list = { NULL, 0 };
  if (s->count > 0) {
    param_t *items = parser_alloc(arena, sizeof(param_t) * s->count);
    memcpy(items, s->items, sizeof(param_t) * s->count);
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
  if ((type = token_type_to_parser_type(CUR_TOK.type)) == -1) {
    GENERATE_ERROR(UNEXPECTED_TOKEN, CUR_TOK, "type name (u8, i8, u16, i16, or void)", "type annotation");
    return res;
  }

  res.kind = (type_kind_t)type;

  ++p->pos; // consume type

  // check for pointer
  while (CUR_TOK.type == s_star && CUR_TOK.type != t_eof) {
    res.is_ptr = 1;
    ++res.ptr_depth;
    ++p->pos; // consume star
  }

  return res;
}


/* 
 * used in both primary() and parse_stmt
 * parses the call args, expects function identifier to already be consumed
 * parses (arg1, arg2, ...) section of call site 
*/
static node_t *parse_function_call_site(parser_t *p, char *name) {
  node_t *n = ALLOC_NODE(p);
  n->kind = NODE_CALL;
  n->call.name = name;

  EXPECT_SYMBOL(s_lparen, "'(' to open argument list", "function call");
  GUARD(p);
  ++p->pos;

  scratch_t scratch = { 0 };

  while (CUR_TOK.type != s_rparen && CUR_TOK.type != t_eof) {
    node_t *arg = logical_or(p);
    GUARD(p);

    scratch_push(&scratch, arg);
    if (CUR_TOK.type == s_comma) ++p->pos;
  }

  n->call.args = scratch_commit(&scratch, p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rparen, "')' to close argument list", "function call");
  GUARD(p);
  ++p->pos;

  return n;
}


/* consumes token, returning next one -> throws error if EOF is encountered */
static token_t consume(parser_t *p) {
  if (p->pos >= p->count || CUR_TOK.type == t_eof) {
    GENERATE_ERROR(UNEXPECTED_EOF, CUR_TOK, "expression (literal, identifier, or '(')", "expression parsing");
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

        EXPECT_SYMBOL(s_rparen, "')' to close cast expression", "cast expression");
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

        EXPECT_SYMBOL(s_rparen, "')' to close grouped expression", "grouped expression");
        GUARD(p);
        ++p->pos; // consume )

        return expr;
      }
    }

    case l_identifier: {
      if (CUR_TOK.type == s_lparen) {
        return parse_function_call_site(p, (char*)tok.value); // error auto propogates, GUARD() return null on error, so will this as is
      } else {
        node_t *ident = ALLOC_NODE(p);
        ident->kind = NODE_IDENTIFIER;
        ident->identifier = (char*)tok.value;;
        return ident;
      }
    }

    default:
      GENERATE_ERROR(UNEXPECTED_TOKEN, tok, "expression (literal, identifier, function call, or '(' expr ')')", "expression parsing");
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


static param_list_t parse_function_params(parser_t *p) {
  param_list_t params = { 0 };

  EXPECT_SYMBOL(s_lparen, "'(' to open parameter list", "function declaration");
  if (p->err) return params;
  ++p->pos;

  param_scratch_t scratch = { 0 };

  while (CUR_TOK.type != s_rparen && CUR_TOK.type != t_eof) {
    // parse each param: type + identifier
    type_t type = parse_type(p);
    if (p->err) { free(scratch.items); return params; }

    EXPECT_SYMBOL(l_identifier, "parameter name", "function parameter list");
    if (p->err) { free(scratch.items); return params; }

    param_t param = {
      .type = type,
      .name = (char*)CUR_TOK.value
    };

    ++p->pos; // consume identifier

    param_scratch_push(&scratch, param);
    if (CUR_TOK.type == s_comma) ++p->pos;
  }

  params = param_scratch_commit(&scratch, p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rparen, "')' to close parameter list", "function declaration");
  if (p->err) return params;
  ++p->pos;

  return params;
}


static node_t *parse_stmt(parser_t *);


static node_list_t parse_block(parser_t *p) {
  node_list_t block = { 0 };
  
  EXPECT_SYMBOL(s_lbrace, "'{' to open block", "block")
  if (p->err) { return block; }
  ++p->pos; // consume {
    
  scratch_t scratch = { 0 };

  // loop through block
  while (CUR_TOK.type != s_rbrace && CUR_TOK.type != t_eof) {
    node_t *statement = parse_stmt(p);
    if (p->err) { free(scratch.items); return block; }

    scratch_push(&scratch, statement);
  }

  block = scratch_commit(&scratch, p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rbrace, "'}' to close block", "block")
  if (p->err) return block;
  ++p->pos;

  return block;
}


static node_t *parse_assignment(parser_t *p) {
  if (p->pos + 1 < p->count) {
    node_t *n;
    op_t op = 0; // used for compound assignments

    switch (p->tokens[p->pos + 1].type) {
      case s_equals: { // normal assignment
        n = ALLOC_NODE(p);
        n->kind = NODE_ASSIGN;
        n->assign.name = (char*)CUR_TOK.value;

        p->pos += 2; // consume identifier, then =
        n->assign.value = parse_expr(p);
        GUARD(p);

        return n;
      }

      // compound assignments
      case s_plus_equals: {
        n = ALLOC_NODE(p);
        n->kind = NODE_ASSIGN;
        
        op = OP_PLUS;
        break;
      }

      case s_minus_equals: {
        n = ALLOC_NODE(p);
        n->kind = NODE_ASSIGN;
        
        op = OP_MINUS;
        break;
      }

      case s_star_equals: {
        n = ALLOC_NODE(p);
        n->kind = NODE_ASSIGN;
        
        op = OP_MULTIPLY;
        break;
      }

      case s_divide_equals: {
        n = ALLOC_NODE(p);
        n->kind = NODE_ASSIGN;
        
        op = OP_DIVIDE;
        break;
      }

      default: return NULL; // not assignment or compound assignment
    }

    // finish populating compound assignment
    n->assign.name = (char*)CUR_TOK.value;
    p->pos += 2; // consume identifier, then +=

    node_t *lhs = ALLOC_NODE(p);
    lhs->kind = NODE_IDENTIFIER;
    lhs->identifier = n->assign.name;

    node_t *rhs = parse_expr(p);
    GUARD(p);

    node_t *binop = ALLOC_NODE(p);
    binop->kind = NODE_BINOP;

    binop->binop.left = lhs;
    binop->binop.op = op;
    binop->binop.right = rhs;

    n->assign.value = binop;

    return n;
  }

  // EOF encountered
  return NULL;
}


static node_t *parse_for_initializer_clause(parser_t *p) {
  if (is_token_type_name(p)) {
    node_t *n = ALLOC_NODE(p);
    n->kind = NODE_VAR_DECL;
    n->var_decl.type = parse_type(p);
    GUARD(p);

    EXPECT_SYMBOL(l_identifier, "variable name", "for loop initialiser");
    GUARD(p);

    n->var_decl.name = (char*)CUR_TOK.value;
    ++p->pos; // consume identifier

    if (CUR_TOK.type == s_equals) {
      ++p->pos; // consume =
      n->var_decl.initialiser = parse_expr(p);
      GUARD(p);
    } else {
      n->var_decl.initialiser = NULL;
    }

    return n;
  }

  return parse_expr(p);
}


static node_t *parse_stmt(parser_t *p) {
  switch (CUR_TOK.type) {
    case Kw_return: {
      node_t *n = ALLOC_NODE(p);

      ++p->pos; // consume return
      n->kind = NODE_RETURN;

      if (CUR_TOK.type == s_semicolon) {
        n->return_val = NULL;
      } else {
        n->return_val = parse_expr(p);
        GUARD(p);
      }

      EXPECT_SYMBOL(s_semicolon, "';' after return value", "return statement");
      GUARD(p);
      ++p->pos; // consume semicolon

      return n;
    }

    case Kw_while: {
      node_t *n = ALLOC_NODE(p);

      ++p->pos; // consume while
      n->kind = NODE_WHILE;

      EXPECT_SYMBOL(s_lparen, "'(' after while", "while statement");
      GUARD(p);
      ++p->pos;

      n->while_stmt.cond = parse_expr(p);
      GUARD(p);

      EXPECT_SYMBOL(s_rparen, "')' to close while condition", "while statement");
      GUARD(p);
      ++p->pos;

      n->while_stmt.body = parse_block(p);
      GUARD(p);

      return n;
    }

    case Kw_for: {
      node_t *n = ALLOC_NODE(p);

      ++p->pos; // consume for
      n->kind = NODE_FOR;

      EXPECT_SYMBOL(s_lparen, "'(' after for", "for statement");
      GUARD(p);
      ++p->pos;

      if (CUR_TOK.type == s_semicolon) {
        n->for_stmt.initialiser = NULL;
        ++p->pos; // consume semicolon
      } else {
        n->for_stmt.initialiser = parse_for_initializer_clause(p);
        GUARD(p);

        EXPECT_SYMBOL(s_semicolon, "';' after for loop initialiser", "for statement");
        GUARD(p);
        ++p->pos; // consume semicolon
      }

      if (CUR_TOK.type == s_semicolon) {
        n->for_stmt.cond = NULL;
        ++p->pos; // consume semicolon
      } else {
        n->for_stmt.cond = parse_expr(p);
        GUARD(p);

        EXPECT_SYMBOL(s_semicolon, "';' after for loop condition", "for statement");
        GUARD(p);
        ++p->pos; // consume semicolon
      }

      if (CUR_TOK.type == s_rparen) {
        n->for_stmt.incrementer = NULL;
        ++p->pos; // consume )
      } else {
        n->for_stmt.incrementer = parse_assignment(p);
        GUARD(p);

        EXPECT_SYMBOL(s_rparen, "')' to close for statement", "for statement");
        GUARD(p);
        ++p->pos; // consume )
      }

      // check for empty body
      if (CUR_TOK.type != s_semicolon) {
        n->for_stmt.body = parse_block(p);
        GUARD(p);
      } else {
        ++p->pos; // consume ;
      }

      return n;
    }

    case Kw_if: {
      node_t *n = ALLOC_NODE(p);

      ++p->pos; // consume if
      n->kind = NODE_IF;

      EXPECT_SYMBOL(s_lparen, "'(' after if", "if statement");
      GUARD(p);
      ++p->pos;

      n->if_stmt.cond = parse_expr(p);
      GUARD(p);

      EXPECT_SYMBOL(s_rparen, "')' to close if condition", "if statement");
      GUARD(p);
      ++p->pos;

      n->if_stmt.then_block = parse_block(p);
      GUARD(p);

      if (CUR_TOK.type == Kw_else) {
        ++p->pos; // consume else
        n->if_stmt.else_block = parse_block(p);
        GUARD(p);
      } else {
        n->if_stmt.else_block.count = 0;
      }

      return n;
    }

    case t_u8: case t_i8: case t_u16: case t_i16: case Kw_void: {
      node_t *n = ALLOC_NODE(p);

      n->kind = NODE_VAR_DECL;
      n->var_decl.type = parse_type(p);
      GUARD(p);

      EXPECT_SYMBOL(l_identifier, "variable name", "variable declaration after type")
      GUARD(p);

      n->var_decl.name = (char*)CUR_TOK.value;
      ++p->pos; // consume identifier

      if (CUR_TOK.type == s_equals) {
        ++p->pos; // consume =
        n->var_decl.initialiser = parse_expr(p);
        GUARD(p);
      } else {
        n->var_decl.initialiser = NULL;
      }

      EXPECT_SYMBOL(s_semicolon, "';' or '= <initialiser>' after variable name", "variable declaration");
      GUARD(p);
      ++p->pos; // consume semicolon

      return n;
    }

    case s_star: {
      node_t *n = ALLOC_NODE(p);

      ++p->pos; // consume *
      n->kind = NODE_DEREF_ASSIGN;

      n->deref_assign.target = parse_expr(p);
      GUARD(p);

      EXPECT_SYMBOL(s_equals, "'=' after dereference target", "dereference assignment");
      GUARD(p);
      ++p->pos; // consume =

      n->deref_assign.value = parse_expr(p);
      GUARD(p);

      EXPECT_SYMBOL(s_semicolon, "';' after dereference assignment", "dereference assignment");
      GUARD(p);
      ++p->pos; // consume semicolon

      return n;
    }

    case l_identifier: {
      node_t *n = parse_assignment(p);
      if (n) {
        EXPECT_SYMBOL(s_semicolon, "';' after assignment", "assignment statement");
        GUARD(p);
        ++p->pos;

        return n;
      }

      // catch if parse_assignment returned NULL due to EOF token (error)
      if (CUR_TOK.type == t_eof) {
        GENERATE_ERROR(UNEXPECTED_EOF, CUR_TOK, "'=', compound assignment operator, or '(' for a function call", "statement");
        return NULL;
      }

      // if we reach here it has to be a function call
      char *name = (char*)CUR_TOK.value;
      ++p->pos; // consume identifier

      n = parse_function_call_site(p, name);
      GUARD(p);

      EXPECT_SYMBOL(s_semicolon, "';' after function call", "function call statement");
      GUARD(p);
      ++p->pos;

      return n;
    }

    default: {
      GENERATE_ERROR(UNEXPECTED_TOKEN, CUR_TOK, "statement (return, if, while, for, variable declaration, assignment, or function call)", "statement");
      return NULL;
    }
  }
}


static node_t *parse_function(parser_t *p) {
  p->pos++; // consume fn keyword

  node_t *func_decl = ALLOC_NODE(p);
  func_decl->kind = NODE_FUNCTION;


  EXPECT_SYMBOL(l_identifier, "function name", "function declaration")
  GUARD(p);

  // pull identifier from token array
  func_decl->function.name = (char*)CUR_TOK.value;
  ++p->pos; // consume identifier

  // parse args
  func_decl->function.params = parse_function_params(p);
  GUARD(p);

  EXPECT_SYMBOL(s_arrow, "'->' before return type", "function declaration")
  GUARD(p);
  ++p->pos; // consume ->

  func_decl->function.return_type = parse_type(p);
  GUARD(p);

  // parse block
  func_decl->function.body = parse_block(p);
  GUARD(p);

  return func_decl;
}


static node_t *parse_reg_decl(parser_t *p) {
  ++p->pos; // consume reg keyword

  node_t *decl = ALLOC_NODE(p);
  decl->kind = NODE_REG_DECL;

  type_t type = parse_type(p);  // consumes type token
  GUARD(p);

  decl->reg_decl.type = type;

  EXPECT_SYMBOL(l_identifier, "register name", "register declaration")
  GUARD(p);

  // pull identifier from token array
  decl->reg_decl.name = (char*)CUR_TOK.value;
  ++p->pos; // consume identifier

  EXPECT_SYMBOL(s_mem_lookup, "'@' before memory address", "register declaration")
  GUARD(p);
  ++p->pos; // consume @

  EXPECT_SYMBOL(l_num, "memory address literal after '@'", "register declaration")
  GUARD(p);

  // pull integer literal (dereference void* to get the stored unsigned long)
  decl->reg_decl.addr = *(unsigned long*)CUR_TOK.value;
  ++p->pos; // consume integer literal

  EXPECT_SYMBOL(s_semicolon, "';' after register declaration", "register declaration")
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

  EXPECT_SYMBOL(l_identifier, "variable name", "global variable declaration")
  GUARD(p);

  // pull identifier from token array
  decl->global_var.name = (char*)CUR_TOK.value;
  ++p->pos; // consume identifier

  if (CUR_TOK.type == s_equals) { // has initialiser, parse it
    ++p->pos; // consume =
    decl->global_var.initialiser = parse_expr(p);
    GUARD(p);
  }

  EXPECT_SYMBOL(s_semicolon, "';' or '= <initialiser>' after variable name", "global variable declaration")
  GUARD(p);
  ++p->pos; // consume ;

  return decl;
}


static node_t *parse_toplevel(parser_t *p) {
  token_t tok = CUR_TOK; 

  switch (tok.type) {
    case Kw_fn:  return parse_function(p);
    case Kw_reg: return parse_reg_decl(p);
    case t_u8: case t_i8: case t_u16: case t_i16: case Kw_void:
      return parse_global_var_decl(p);

    default:
      GENERATE_ERROR(UNEXPECTED_TOKEN, tok, "top-level declaration (fn, reg, or type name for a global variable)", "top-level parse");
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