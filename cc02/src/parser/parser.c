/*
 * parser.c - C02 recursive descent parser
 *
 * OVERVIEW
 * --------
 * Converts a flat token array (from tokenizer.c) into an AST rooted at a
 * NODE_PROGRAM node. All AST nodes are allocated from a chunked arena
 * (arena_t, see arena.h) — call parser_init() before parse() and
 * parser_free() when done. String fields in nodes (names, identifiers)
 * point directly into the token array; the token array must outlive the AST.
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
 *   s_star       -> NODE_ASSIGN
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
 * field_t using a parallel field_scratch_t.
 *
 * TOP-LEVEL PARSING
 * -----------------
 * parse_toplevel() dispatches on:
 *   Kw_fn        -> parse_function()   (name + params + return type + block)
 *   Kw_reg       -> parse_reg_decl()   (type + name + @ + address)
 *   Kw_struct    -> parse_struct_decl()
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


int parser_init(parser_t *p) {
  size_t chunk_size = (PARSER_CHUNK_ALLOC_SIZE + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
  return arena_init(&p->arena, chunk_size);
}


void parser_free(parser_t *p) {
  if (p) {
    arena_free(&p->arena);
  }
}

// ----------------------------------------------------------------
// Define scratch functions
// ----------------------------------------------------------------

#include "parser_macros.inc"

DEFINE_NODE_SCRATCH(scratch)                                              // node_t* — statement lists, call args, etc.
DEFINE_VALUE_SCRATCH(field_scratch, field_t, field_list_t)                // struct decl fields
DEFINE_VALUE_SCRATCH(param_scratch, param_t, param_list_t)                // function params
DEFINE_VALUE_SCRATCH(field_init_scratch, field_init_t, field_init_list_t) // struct initializer { .field = val }
DEFINE_VALUE_SCRATCH(asm_scratch, char*, asm_line_list_t)                 // asm {} block mnemonics


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


// Allocates a zeroed node_t from p's arena and stamps its source location
// from whatever token is current - this is "where parsing of this node
// began", not necessarily the node's full span (e.g. a NODE_BINOP's loc is
// wherever its left operand started, not the operator or right side), but
// it's enough to point an error at the right neighbourhood of source.
static node_t *alloc_node(parser_t *p) {
  node_t *n = ARENA_ALLOC(&p->arena, node_t);
  n->loc = CUR_TOK.loc;
  return n;
}
#define ALLOC_NODE(p) alloc_node(p)


static node_t *parse_struct_init(parser_t *p, char *struct_name);
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
    if (CUR_TOK.type == l_identifier) {
      res.kind = TYPE_STRUCT;
      res.struct_name = CUR_TOK.string_val;
      ++p->pos; // consume identifier
    } else {
      GENERATE_ERROR(ERR_UNEXPECTED_TOKEN, CUR_TOK, "type name (u8, i8, u16, i16, void, or struct name)", "type annotation");
      return res;
    }
  } else {
    res.kind = (type_kind_t)type;
    ++p->pos; // consume type
  }

  while (CUR_TOK.type == s_star && CUR_TOK.type != t_eof) {
    res.is_ptr = 1;
    ++res.ptr_depth;
    ++p->pos;
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

    scratch_push(&scratch, arg, p);
    GUARD(p);
    if (CUR_TOK.type == s_comma) ++p->pos;
  }

  n->call.args = scratch_commit(&scratch, &p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rparen, "')' to close argument list", "function call");
  GUARD(p);
  ++p->pos;

  return n;
}


/* consumes token, returning next one -> throws error if EOF is encountered */
static token_t consume(parser_t *p) {
  if (p->pos >= p->count || CUR_TOK.type == t_eof) {
    GENERATE_ERROR(ERR_UNEXPECTED_EOF, CUR_TOK, "expression (literal, identifier, or '(')", "expression parsing");
    return CUR_TOK; // return whatever's there, caller checks p->err
  }

  return p->tokens[p->pos++];
}


static node_t *primary(parser_t *p) {
  token_t tok = consume(p);
  GUARD(p);

  node_t *expr;

  switch (tok.type) {
    case l_num: { // literal
      expr = ALLOC_NODE(p);
      expr->kind = NODE_NUMBER;
      expr->number.value = tok.num_val;
      break;
    }

    case l_string: {
      expr = ALLOC_NODE(p);
      expr->kind = NODE_STRING;
      expr->value = tok.string_val;
      break;
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

        expr = ALLOC_NODE(p);
        expr->kind = NODE_CAST;
        expr->cast.cast_type = t;
        expr->cast.operand = rhs;

        break;
      } else {
        expr = logical_or(p);

        EXPECT_SYMBOL(s_rparen, "')' to close grouped expression", "grouped expression");
        GUARD(p);
        ++p->pos; // consume )

        break;
      }
    }

    case l_identifier: {
      if (CUR_TOK.type == s_lparen) {
        expr = parse_function_call_site(p, tok.string_val);
        break;
      } else if (CUR_TOK.type == s_lbrace) {
        expr = parse_struct_init(p, tok.string_val);
        break;
      } else {
        node_t *ident = ALLOC_NODE(p);
        ident->kind = NODE_IDENTIFIER;
        ident->identifier.name = tok.string_val;
        expr = ident;
        break;
      }
    }

    default:
      GENERATE_ERROR(ERR_UNEXPECTED_TOKEN, tok, "expression (literal, identifier, function call, or '(' expr ')')", "expression parsing");
      return NULL;
  }

  GUARD(p);

  // postfix field access, applies to whatever expr came out above
  while (CUR_TOK.type == s_dot) {
    ++p->pos;
    EXPECT_SYMBOL(l_identifier, "field name", "field access");
    GUARD(p);

    node_t *access = ALLOC_NODE(p);
    access->kind = NODE_FIELD_ACCESS;
    access->field_access.base = expr;
    access->field_access.field = CUR_TOK.string_val;
    ++p->pos;

    expr = access;
  }

  return expr;
}


static node_t *unary(parser_t *p) {
  op_t op;
  switch (CUR_TOK.type) {
    case s_bang:        op = OP_BANG; break;
    case s_minus:       op = OP_NEGATE; break;
    case s_ampersand:   op = OP_ADDRESSOF; break;
    case s_plus_plus:   op = OP_INCREMENT; break;
    case s_minus_minus: op = OP_DECREMENT; break;
    case s_not:         op = OP_BNOT; break;

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

  // reaching here means its a standard unary op, not a pecial case
  ++p->pos; // consume unary op
  node_t *operand = unary(p);
  GUARD(p);

  node_t *n = ALLOC_NODE(p);
  n->kind = NODE_UNARY;
  n->unary.op = op;
  n->unary.operand = operand;

  return n;
}


BINOP_LEVEL(factor, unary,
  case s_star:    op = OP_MULTIPLY; break;
  case s_divide:  op = OP_DIVIDE;   break;
  case s_modulus: op = OP_MODULUS;  break;
)

BINOP_LEVEL(term, factor,
  case s_plus:  op = OP_PLUS;  break;
  case s_minus: op = OP_MINUS; break;
)

BINOP_LEVEL(shift, term,
  case s_l_shift: op = OP_LEFT_SHIFT; break;
  case s_r_shift: op = OP_RIGHT_SHIFT; break;
)

BINOP_LEVEL(comparison, shift,
  case s_lt:  op = OP_LT;  break;
  case s_lte: op = OP_LTE; break;
  case s_gt:  op = OP_GT;  break;
  case s_gte: op = OP_GTE; break;
)

BINOP_LEVEL(equality, comparison,
  case s_equals_equals: op = OP_EQUALSEQUALS; break;
  case s_bang_equals:   op = OP_BANGEQUALS;   break;
)

BINOP_LEVEL(bitwise_and, equality,
  case s_ampersand: op = OP_BAND; break;
)

BINOP_LEVEL(bitwise_xor, bitwise_and,
  case s_caret: op = OP_BXOR; break;
)

BINOP_LEVEL(bitwise_or, bitwise_xor,
  case s_pipe: op = OP_BOR; break;
)

BINOP_LEVEL(logical_and, bitwise_or,
  case s_and: op = OP_AND; break;
)

// root of recursive descent
// logical_or   looks for ||            calls logical_and
// logical_and  looks for &&            calls equality
// bitwise_or   looks for |             calls bitwise_xor
// bitwise_xor  looks for ^             calls bitwise_and
// bitwise_and  looks for &             calls equality
// equality     looks for == !=         calls comparison
// comparison   looks for < > <= >=     calls shift
// shift        looks for << >>         calls term
// term         looks for + -           calls factor
// factor       looks for * / %         calls unary
// unary        looks for * @ ! - ++ -- calls primary
// primary      looks for literals, identifiers, (expr)   consumes tokens
BINOP_LEVEL(logical_or, logical_and,
  case s_or: op = OP_OR; break;
)


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

    field_t param = {
      .type = type,
      .name = CUR_TOK.string_val
    };

    ++p->pos; // consume identifier

    param_scratch_push(&scratch, param, p);
    if (p->err) { free(scratch.items); return params; }
    if (CUR_TOK.type == s_comma) ++p->pos;
  }

  params = param_scratch_commit(&scratch, &p->arena);
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

    scratch_push(&scratch, statement, p);
    if (p->err) { free(scratch.items); return block; }
  }

  block = scratch_commit(&scratch, &p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rbrace, "'}' to close block", "block")
  if (p->err) return block;
  ++p->pos;

  return block;
}


static node_t *parse_block_node(parser_t *p) {
  node_t *block = ALLOC_NODE(p);
  block->kind = NODE_BLOCK;

  block->block = parse_block(p);
  GUARD(p);

  return block;
}


static node_t *parse_assignment(parser_t *p) {
  node_t *target = parse_expr(p);  // handles identifier, field access, chains
  GUARD(p);

  op_t op = 0;
  switch (CUR_TOK.type) {
    case s_equals: {
      ++p->pos;
      node_t *n = ALLOC_NODE(p);
      n->kind = NODE_ASSIGN;
      n->assign.target = target;
      n->assign.value = parse_expr(p);
      GUARD(p);
      return n;
    }
    case s_plus_equals:    op = OP_PLUS;     break;
    case s_minus_equals:   op = OP_MINUS;    break;
    case s_star_equals:    op = OP_MULTIPLY; break;
    case s_divide_equals:  op = OP_DIVIDE;   break;
    case s_modulus_equals: op = OP_MODULUS;  break;
    default: return target; // not an assignment, just an expression statement
  }

  ++p->pos; // consume compound-assign operator
  node_t *n = ALLOC_NODE(p);
  n->kind = NODE_ASSIGN;
  n->assign.target = target;

  node_t *rhs = parse_expr(p);
  GUARD(p);

  node_t *binop = ALLOC_NODE(p);
  binop->kind = NODE_BINOP;
  binop->binop.left = target;   // reuse target as lhs of the binop too
  binop->binop.op = op;
  binop->binop.right = rhs;

  n->assign.value = binop;
  return n;
}

static node_t *parse_for_initializer_clause(parser_t *p) {
  if (is_token_type_name(p)) {
    node_t *n = ALLOC_NODE(p);
    n->kind = NODE_VAR_DECL;
    n->var_decl.type = parse_type(p);
    GUARD(p);

    EXPECT_SYMBOL(l_identifier, "variable name", "for loop initialiser");
    GUARD(p);

    n->var_decl.name = CUR_TOK.string_val;
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


static node_t *parse_struct_decl(parser_t *p) {
  ++p->pos; // consume struct keyword

  node_t *decl = ALLOC_NODE(p);
  decl->kind = NODE_STRUCT_DECL;

  EXPECT_SYMBOL(l_identifier, "struct name", "struct declaration");
  GUARD(p);

  decl->struct_decl.name = CUR_TOK.string_val;
  ++p->pos; // consume identifier

  EXPECT_SYMBOL(s_lbrace, "'{' to open struct body", "struct declaration");
  GUARD(p);
  ++p->pos; // consume {

  field_scratch_t scratch = { 0 };

  while (CUR_TOK.type != s_rbrace && CUR_TOK.type != t_eof) {
    type_t type = parse_type(p);
    if (p->err) { free(scratch.items); return NULL; }

    EXPECT_SYMBOL(l_identifier, "field name", "struct field");
    if (p->err) { free(scratch.items); return NULL; }

    field_t field = {
      .type = type,
      .name = CUR_TOK.string_val
    };
    ++p->pos; // consume identifier

    field_scratch_push(&scratch, field, p);
    if (p->err) { free(scratch.items); return NULL; }

    EXPECT_SYMBOL(s_semicolon, "';' after struct field", "struct field");
    if (p->err) { free(scratch.items); return NULL; }
    ++p->pos; // consume ;
  }

  decl->struct_decl.fields = field_scratch_commit(&scratch, &p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rbrace, "'}' to close struct body", "struct declaration");
  GUARD(p);
  ++p->pos; // consume }

  // A trailing ';' is optional: the README writes `struct Foo { ... };`, but
  // the existing corpus omits it, so accept either rather than forcing one.
  // This is the single entry point for struct decls, so it covers both
  // top-level and in-block declarations.
  if (CUR_TOK.type == s_semicolon) ++p->pos;

  return decl;
}


static node_t *parse_struct_init(parser_t *p, char *struct_name) {
  node_t *n = ALLOC_NODE(p);
  n->kind = NODE_STRUCT_INIT;
  n->struct_init.struct_name = struct_name;

  EXPECT_SYMBOL(s_lbrace, "'{' to open struct initializer", "struct initializer");
  GUARD(p);
  ++p->pos; // consume {

  field_init_scratch_t scratch = { 0 };

  while (CUR_TOK.type != s_rbrace && CUR_TOK.type != t_eof) {
    EXPECT_SYMBOL(s_dot, "'.' before field name", "struct initializer field");
    if (p->err) { free(scratch.items); return NULL; }
    ++p->pos; // consume .

    EXPECT_SYMBOL(l_identifier, "field name", "struct initializer field");
    if (p->err) { free(scratch.items); return NULL; }

    char *field_name = CUR_TOK.string_val;
    ++p->pos; // consume field name

    EXPECT_SYMBOL(s_equals, "'=' after field name", "struct initializer field");
    if (p->err) { free(scratch.items); return NULL; }
    ++p->pos; // consume =

    node_t *value = parse_expr(p);
    if (p->err) { free(scratch.items); return NULL; }

    field_init_t init = { .field_name = field_name, .value = value };
    field_init_scratch_push(&scratch, init, p);
    if (p->err) { free(scratch.items); return NULL; }

    if (CUR_TOK.type == s_comma) ++p->pos;
  }

  n->struct_init.inits = field_init_scratch_commit(&scratch, &p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rbrace, "'}' to close struct initializer", "struct initializer");
  GUARD(p);
  ++p->pos; // consume }

  return n;
}


// Parses `asm { MNEMONIC ... }` - a bare list of opcode mnemonics, one per
// token, no operands/addressing modes/labels. Codegen owns the table of
// which mnemonics are actually valid; the parser just collects identifiers.
static node_t *parse_asm_block(parser_t *p) {
  node_t *n = ALLOC_NODE(p);
  n->kind = NODE_ASM_BLOCK;

  ++p->pos; // consume asm

  EXPECT_SYMBOL(s_lbrace, "'{' to open asm block", "asm block");
  GUARD(p);
  ++p->pos; // consume {

  asm_scratch_t scratch = { 0 };

  while (CUR_TOK.type != s_rbrace && CUR_TOK.type != t_eof) {
    EXPECT_SYMBOL(l_identifier, "opcode mnemonic", "asm block");
    if (p->err) { free(scratch.items); return NULL; }

    asm_scratch_push(&scratch, CUR_TOK.string_val, p);
    if (p->err) { free(scratch.items); return NULL; }
    ++p->pos;
  }

  n->asm_block = asm_scratch_commit(&scratch, &p->arena);
  free(scratch.items);

  EXPECT_SYMBOL(s_rbrace, "'}' to close asm block", "asm block");
  GUARD(p);
  ++p->pos; // consume }

  // if next token is a semicolon, consume it. This is optional
  if (CUR_TOK.type == s_semicolon) ++p->pos;

  return n;
}


static node_t *parse_stmt(parser_t *p) {
  switch (CUR_TOK.type) {
    case Kw_asm: return parse_asm_block(p);
    case Kw_struct: return parse_struct_decl(p);

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

      // check for empty body
      if (CUR_TOK.type != s_semicolon) {
        n->while_stmt.body = parse_block_node(p);
        GUARD(p);
      } else {
        ++p->pos; // consume ;
      }

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
        n->for_stmt.body = parse_block_node(p);
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

      scratch_t scratch = { 0 };
      scratch_push(&scratch, parse_block_node(p), p); // parse then block
      GUARD(p);

      while (CUR_TOK.type == Kw_else) {
        ++p->pos; // consume else

        if (CUR_TOK.type == Kw_if) {
          scratch_push(&scratch, parse_stmt(p), p);
          GUARD(p);
        } else {
          scratch_push(&scratch, parse_block_node(p), p);
          GUARD(p);
        }
      }

      n->if_stmt.blocks = scratch_commit(&scratch, &p->arena);
      free(scratch.items);

      return n;
    }

    case Kw_break: {
      node_t *n = alloc_node(p);

      n->kind = NODE_BREAK;
      ++p->pos;

      EXPECT_SYMBOL(s_semicolon, ";", "after break statement.");
      GUARD(p);
      ++p->pos;

      return n;
    }

    case Kw_continue: {
      node_t *n = alloc_node(p);

      n->kind = NODE_CONTINUE;
      ++p->pos;

      EXPECT_SYMBOL(s_semicolon, ";", "after continue statement.");
      GUARD(p);
      ++p->pos;

      return n;
    }

    case t_u8: case t_i8: case t_u16: case t_i16: case Kw_void: {
      node_t *n = ALLOC_NODE(p);

      n->kind = NODE_VAR_DECL;
      n->var_decl.type = parse_type(p);
      GUARD(p);

      EXPECT_SYMBOL(l_identifier, "variable name", "variable declaration after type")
      GUARD(p);

      n->var_decl.name = CUR_TOK.string_val;
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

    case s_plus_plus:
    case s_minus_minus: {
      node_t *n = parse_expr(p);
      GUARD(p);

      EXPECT_SYMBOL(s_semicolon, "';' after expression", "expression statement");
      GUARD(p);
      ++p->pos; // consume ;

      return n;
    }

    case s_star: {
      node_t *n = parse_assignment(p);
      GUARD(p);

      EXPECT_SYMBOL(s_semicolon, "';' after statement", "expression statement");
      GUARD(p);
      ++p->pos;

      return n;
    }

    case l_identifier: {
      // disambiguate struct-typed var decl ("Engine* e;", "Point p;") from
      // assignment/call ("p = ...", "p();") by scanning past any pointer
      // stars to see if an identifier follows — that pattern only occurs
      // in a declaration, never in an expression.
      unsigned lookahead = p->pos + 1;
      while (lookahead < p->count && p->tokens[lookahead].type == s_star) {
        ++lookahead;
      }

      if (lookahead < p->count && p->tokens[lookahead].type == l_identifier) {
        node_t *n = ALLOC_NODE(p);
        n->kind = NODE_VAR_DECL;
        n->var_decl.type = parse_type(p);
        GUARD(p);

        EXPECT_SYMBOL(l_identifier, "variable name", "variable declaration after type");
        GUARD(p);

        n->var_decl.name = CUR_TOK.string_val;
        ++p->pos;

        if (CUR_TOK.type == s_equals) {
          ++p->pos;
          n->var_decl.initialiser = parse_expr(p);
          GUARD(p);
        } else {
          n->var_decl.initialiser = NULL;
        }

        EXPECT_SYMBOL(s_semicolon, "';' after variable declaration", "variable declaration");
        GUARD(p);
        ++p->pos;

        return n;
      }

      // otherwise: assignment or function call statement
      node_t *n = parse_assignment(p);
      GUARD(p);

      EXPECT_SYMBOL(s_semicolon, "';' after statement", "expression statement");
      GUARD(p);
      ++p->pos;

      return n;
    }
    default: {
      GENERATE_ERROR(ERR_UNEXPECTED_TOKEN, CUR_TOK, "statement (return, if, while, for, variable declaration, assignment, or function call)", "statement");
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
  func_decl->function.name = CUR_TOK.string_val;
  ++p->pos; // consume identifier

  // parse args
  func_decl->function.params = parse_function_params(p);
  GUARD(p);

  // an `interrupt` function gets register-save/RTI codegen instead of a plain RTS
  func_decl->function.is_interrupt = (CUR_TOK.type == Kw_interrupt);
  if (func_decl->function.is_interrupt) {
    ++p->pos; // consume interrupt
  }

  EXPECT_SYMBOL(s_arrow, "'->' before return type", "function declaration")
  GUARD(p);
  ++p->pos; // consume ->

  func_decl->function.return_type = parse_type(p);
  GUARD(p);

  // parse block
  func_decl->function.body = parse_block_node(p);
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
  decl->reg_decl.name = CUR_TOK.string_val;
  ++p->pos; // consume identifier

  EXPECT_SYMBOL(s_mem_lookup, "'@' before memory address", "register declaration")
  GUARD(p);
  ++p->pos; // consume @

  EXPECT_SYMBOL(l_num, "memory address literal after '@'", "register declaration")
  GUARD(p);

  // pull integer literal (dereference void* to get the stored unsigned long)
  decl->reg_decl.addr = CUR_TOK.num_val;
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
  decl->global_var.name = CUR_TOK.string_val;
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


static node_t *parse_fwd_decl(parser_t *p) {
  ++p->pos; // consume decl keyword

  node_t *n = ALLOC_NODE(p);
  n->kind = NODE_FWD_DECL;

  if (CUR_TOK.type == Kw_fn) {
    ++p->pos; // consume fn
    n->fwd_decl.is_function = 1;

    EXPECT_SYMBOL(l_identifier, "function name", "forward declaration");
    GUARD(p);

    n->fwd_decl.name = CUR_TOK.string_val;
    ++p->pos; // consume identifier

    n->fwd_decl.params = parse_function_params(p);
    GUARD(p);

    // check if next symbol is interrupt, skip if not, parse return type
    if (CUR_TOK.type == Kw_interrupt) {
      ++p->pos; // consume interrupt
    }

    EXPECT_SYMBOL(s_arrow, "'->' before return type", "forward declaration");
    GUARD(p);
    ++p->pos; // consume ->

    n->fwd_decl.type = parse_type(p);
    GUARD(p);
  } else {
    n->fwd_decl.is_function = 0;

    n->fwd_decl.type = parse_type(p);
    GUARD(p);

    EXPECT_SYMBOL(l_identifier, "variable name", "forward declaration");
    GUARD(p);

    n->fwd_decl.name = CUR_TOK.string_val;
    ++p->pos; // consume identifier

    n->fwd_decl.params = (param_list_t){ 0 };
  }

  EXPECT_SYMBOL(s_semicolon, "';' after forward declaration", "forward declaration");
  GUARD(p);
  ++p->pos; // consume ;

  return n;
}


static node_t *parse_toplevel(parser_t *p) {
  token_t tok = CUR_TOK;

  switch (tok.type) {
    case Kw_fwd_decl: return parse_fwd_decl(p);
    case Kw_fn:     return parse_function(p);
    case Kw_reg:    return parse_reg_decl(p);
    case Kw_struct: return parse_struct_decl(p);
    case t_u8: case t_i8: case t_u16: case t_i16: case Kw_void:
      return parse_global_var_decl(p);

    case l_identifier: {
      // a struct-typed global ("Point p;", "Engine *e;"): a type-name
      // identifier followed (past any '*') by another identifier - the same
      // disambiguation parse_stmt uses for struct-typed locals. The struct's
      // existence is checked later, in semantic analysis.
      unsigned lookahead = p->pos + 1;
      while (lookahead < p->count && p->tokens[lookahead].type == s_star) {
        ++lookahead;
      }
      if (lookahead < p->count && p->tokens[lookahead].type == l_identifier) {
        return parse_global_var_decl(p);
      }
      GENERATE_ERROR(ERR_UNEXPECTED_TOKEN, tok, "top-level declaration (fn, decl, reg, struct, or a typed global variable)", "top-level parse");
      return NULL;
    }

    default:
      GENERATE_ERROR(ERR_UNEXPECTED_TOKEN, tok, "top-level declaration (fn, decl, reg, struct, or a typed global variable)", "top-level parse");
      return NULL;
  }
}


ast_t parse(parser_t *p, token_t *tokens, unsigned num_tokens) {
  p->tokens = tokens;
  p->count = num_tokens; 
  p->pos = p->has_errored = 0;
  p->err = NULL;

  scratch_t scratch = {0};
  while (p->pos < p->count && CUR_TOK.type != t_eof) {
    scratch_push(&scratch, parse_toplevel(p), p);

    if (p->has_errored) {
      print_error(p->err);

      free(scratch.items);
      free(p->err);
      p->err = NULL;
      return NULL;
    }
  }

  node_t *root = ALLOC_NODE(p);
  root->kind = NODE_PROGRAM;
  root->program = scratch_commit(&scratch, &p->arena);

  free(scratch.items);
  return root;
}