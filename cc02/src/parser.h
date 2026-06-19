#ifndef __PARSER_H__
#define __PARSER_H__

#include <stdint.h>
#include <stddef.h>

#include "tokenizer.h"

// ----------------------------------------------------------------
// Types
// ----------------------------------------------------------------

typedef enum {
  TYPE_U8, TYPE_I8, TYPE_U16, TYPE_I16, TYPE_VOID,
} type_kind_t;

typedef struct {
  type_kind_t kind;
  int is_ptr;
  unsigned ptr_depth;
} type_t;

// ----------------------------------------------------------------
// Operators
// ----------------------------------------------------------------

typedef enum {
  OP_INCREMENT, OP_DECREMENT, OP_PLUS, OP_MINUS, OP_MULTIPLY, OP_DIVIDE, OP_MODULUS,
  
  OP_LT, OP_GT, OP_LTE, OP_GTE,
  OP_EQUALSEQUALS, OP_BANGEQUALS,
  OP_BANG, OP_NEGATE, OP_ADDRESSOF, OP_AND, OP_OR,

  OP_LEFT_SHIFT, OP_RIGHT_SHIFT,
  OP_BAND, OP_BXOR, OP_BOR, OP_BNOT
} op_t;

// ----------------------------------------------------------------
// Node
// ----------------------------------------------------------------

// NOTE: string fields in nodes (name, identifier, etc.) point directly into
// the token array. tokens must remain valid for the lifetime of the AST.

typedef enum {
  // literals
  NODE_NUMBER,
  NODE_STRING,
  NODE_IDENTIFIER,

  // expressions
  NODE_BINOP,
  NODE_UNARY,
  NODE_CALL,
  NODE_DEREF,
  NODE_CAST,
  // statements
  NODE_VAR_DECL,
  NODE_ASSIGN,
  NODE_DEREF_ASSIGN,
  NODE_RETURN,
  NODE_IF,
  NODE_WHILE,
  NODE_FOR,
  NODE_BLOCK,
  // top-level
  NODE_FUNCTION,
  NODE_REG_DECL,
  NODE_GLOBAL_VAR,
  // root
  NODE_PROGRAM,
} node_kind_t;

typedef struct node_t node_t;

typedef struct {
  node_t   **items;
  unsigned   count;
} node_list_t;

typedef struct {
  type_t  type;
  char   *name;
} param_t;

typedef struct {
  param_t  *items;
  unsigned  count;
} param_list_t;

struct node_t {
  node_kind_t kind;
  union {
    // --- expressions ---
    long   number;                        // NODE_NUMBER
    char  *value;                         // NODE_STRING
    char  *identifier;                    // NODE_IDENTIFIER

    struct {
      node_t *left;
      op_t    op;
      node_t *right;
    } binop;                              // NODE_BINOP

    struct {
      op_t    op;
      node_t *operand;
    } unary;                              // NODE_UNARY

    struct {
      char       *name;
      node_list_t args;
    } call;                               // NODE_CALL

    node_t *deref_target;                 // NODE_DEREF

    struct {
      type_t  cast_type;
      node_t *operand;
    } cast;                               // NODE_CAST

    // --- statements ---
    struct {
      type_t  type;
      char   *name;
      node_t *initialiser;                // NULL if absent
    } var_decl;                           // NODE_VAR_DECL

    struct {
      char   *name;
      node_t *value;
    } assign;                             // NODE_ASSIGN

    struct {
      node_t *target;
      node_t *value;
    } deref_assign;                       // NODE_DEREF_ASSIGN

    node_t *return_val;                   // NODE_RETURN (NULL for bare return)

    struct {
      node_t     *cond;

      // blocks[0] == then block, after that else if's are if nodes in the list, a bare block node is an else block with no if
      node_list_t blocks;
    } if_stmt;                            // NODE_IF

    struct {
      node_t *cond;
      node_t *body;
    } while_stmt;                         // NODE_WHILE

    struct {
      node_t     *initialiser;
      node_t     *cond;
      node_t     *incrementer;
      node_t     *body;
    } for_stmt;                           // NODE_FOR

    node_list_t block;                    // NODE_BLOCK

    // --- top-level ---
    struct {
      char        *name;
      param_list_t params;
      type_t       return_type;
      node_t      *body;
    } function;                           // NODE_FUNCTION

    struct {
      type_t   type;
      char    *name;
      unsigned long addr;
    } reg_decl;                           // NODE_REG_DECL

    struct {
      type_t  type;
      char   *name;
      node_t *initialiser;                // NULL if absent
    } global_var;                         // NODE_GLOBAL_VAR

    // --- root ---
    node_list_t program;                  // NODE_PROGRAM
  };
};

// ----------------------------------------------------------------
// Memory Management
// ----------------------------------------------------------------

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
} parser_arena_t;

#define PARSER_CHUNK_ALLOC_SIZE (sizeof(node_t) * 100)

/* error reporting types, internal to parsing code */
typedef enum {
  UNEXPECTED_EOF,
  UNEXPECTED_TOKEN,
  ALLOCATION_FAILED
} error_type_t;

typedef struct {
  error_type_t type;
  token_t found;

  char *expected;
  char *context;
} error_t;

// ----------------------------------------------------------------
// General API
// ----------------------------------------------------------------

int parser_init(parser_arena_t *a, size_t size);
void parser_free(parser_arena_t *a);

void print_ast(node_t *node);

node_t *parse(token_t *tokens, unsigned num_tokens, parser_arena_t *mem_area);

#endif // __PARSER_H__