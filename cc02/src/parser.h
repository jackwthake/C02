#ifndef __PARSER_H__
#define __PARSER_H__

#include <stdint.h>
#include <stddef.h>

#include "tokenizer.h"

// ----------------------------------------------------------------
// Types
// ----------------------------------------------------------------

typedef enum {
  TYPE_U8, TYPE_I8, TYPE_U16, TYPE_I16, TYPE_VOID, TYPE_STRUCT
} type_kind_t;

typedef struct {
  type_kind_t kind;
  unsigned is_ptr;
  unsigned ptr_depth;
  char *struct_name;    // only valid when kind == TYPE_STRUCT; resolved to a decl in analysis
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
  NODE_STRUCT_INIT,
  NODE_ASSIGN,
  NODE_RETURN,
  NODE_IF,
  NODE_WHILE,
  NODE_FOR,
  NODE_BLOCK,

  // top-level
  NODE_FUNCTION,
  NODE_REG_DECL,
  NODE_GLOBAL_VAR,

  NODE_STRUCT_DECL,
  NODE_FIELD_ACCESS,
  // root
  NODE_PROGRAM,
} node_kind_t;


// ----------------------------------------------------------------
// Scratch buffer structs
// ----------------------------------------------------------------

typedef struct node_t node_t;

typedef struct {
  node_t   **items;
  unsigned   count;
} node_list_t;

typedef struct {
  type_t  type;
  char   *name;
} scratch_buffer_item_t;

typedef struct {
  scratch_buffer_item_t  *items;
  unsigned  count;
} scratch_buffer_list_t;

typedef scratch_buffer_item_t field_t;
typedef scratch_buffer_list_t field_list_t;

typedef scratch_buffer_item_t param_t;
typedef scratch_buffer_list_t param_list_t;

typedef struct {
  char *field_name;
  node_t *value;
} field_init_t;

typedef struct {
  field_init_t *items;
  unsigned count;
} field_init_list_t;


// ----------------------------------------------------------------
// Node struct
// ----------------------------------------------------------------

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
      char *struct_name;
      field_init_list_t inits;
    } struct_init;                        // NODE_STRUCT_INIT

    struct {
      node_t *target;
      node_t *value;
    } assign;                             // NODE_ASSIGN

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

    struct {
      char *name;
      field_list_t fields;
    } struct_decl;                        // NODE_STRUCT_DECL

    struct {
      node_t *base;     // the expression being accessed (identifier, call result, another field access, etc.)
      char   *field;    // field name token text
    } field_access;

    // --- root ---
    node_list_t program;                  // NODE_PROGRAM
  };
};


typedef node_t * ast_t;


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


// ----------------------------------------------------------------
// Error types
// ----------------------------------------------------------------

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


typedef struct {
  parser_arena_t arena;

  token_t *tokens;
  unsigned count;
  unsigned pos;
  
  unsigned has_errored;
  error_t *err;
} parser_t;


// ----------------------------------------------------------------
// General API
// ----------------------------------------------------------------

int parser_init(parser_t *p);
void parser_free(parser_t *p);

void print_ast(ast_t node);

ast_t parse(parser_t *p, token_t *tokens, unsigned num_tokens);

#endif // __PARSER_H__