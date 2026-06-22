#ifndef __IR_H__
#define __IR_H__

/*
 * ir.h - three-address code (TAC) and control flow graph (CFG)
 * -------------------------------------------------------------
 * The intermediate representation sits between the analysed AST and
 * target code generation. It is designed to be self-contained: codegen
 * should be able to emit target code from the ir_module_t alone,
 * without consulting the AST or symbol table.
 *
 * The AST is lowered into a flat sequence of TAC instructions (at most
 * two sources and one destination each), which is then carved into
 * basic blocks and wired into a CFG - one CFG per function.
 *
 * Register declarations (memory-mapped hardware registers) are lowered
 * to TAC_LOAD / TAC_STORE with the fixed address baked in as an
 * OPERAND_CONST_INT, so codegen never needs to resolve register
 * symbols - it just sees a load/store at an address.
 *
 * TAC OPERANDS
 * ------------
 * Every source/destination in an instruction is a tac_operand_t:
 * a tagged union that can be a compiler-generated temporary, a named
 * variable, an integer constant, or a string literal. Temporaries
 * are numbered (t0, t1, ...) per-function.
 *
 * BASIC BLOCKS & CFG
 * ------------------
 * A basic_block_t is a straight-line run of instructions with one
 * entry point (the first instruction) and one exit (the last, which
 * is always a jump, conditional branch, or return). Blocks are
 * connected by successor edges (0 = return, 1 = unconditional/
 * fall-through, 2 = conditional branch).
 *
 * IR MODULE
 * ---------
 * An ir_module_t is the complete output of IR generation: struct
 * layouts (with computed field offsets and sizes), global variable
 * declarations, register definitions, and one CFG per function.
 * Everything codegen needs is here.
 */

#include "arena.h"
#include "types.h"
#include "parser.h"
#include "analyzer.h"


// ----------------------------------------------------------------
// TAC operand
// ----------------------------------------------------------------

typedef enum {
  OPERAND_NONE,
  OPERAND_TEMP,
  OPERAND_VAR,
  OPERAND_CONST_INT,
  OPERAND_CONST_STR,
} tac_operand_kind_t;

typedef struct {
  tac_operand_kind_t kind;
  type_t type;
  union {
    unsigned temp_id;       // OPERAND_TEMP
    char *name;             // OPERAND_VAR
    long int_val;           // OPERAND_CONST_INT
    char *str_val;          // OPERAND_CONST_STR
  };
} tac_operand_t;


// ----------------------------------------------------------------
// TAC instruction
// ----------------------------------------------------------------

typedef enum {
  // binary arithmetic
  TAC_ADD, TAC_SUB, TAC_MUL, TAC_DIV, TAC_MOD,

  // comparison
  TAC_LT, TAC_GT, TAC_LTE, TAC_GTE, TAC_EQ, TAC_NEQ,

  // logical
  TAC_AND, TAC_OR,

  // bitwise
  TAC_SHL, TAC_SHR, TAC_BAND, TAC_BXOR, TAC_BOR,

  // unary
  TAC_NEG, TAC_NOT, TAC_BNOT,
  TAC_INC,          // dst = dst + 1  (maps to 6502 INC/INX/INY)
  TAC_DEC,          // dst = dst - 1  (maps to 6502 DEC/DEX/DEY)

  // memory / pointers
  TAC_ADDR_OF,      // dst = &src1
  TAC_LOAD,         // dst = *src1  (also used for register reads at fixed addresses)
  TAC_STORE,        // *dst = src1  (also used for register writes at fixed addresses)

  // data movement
  TAC_COPY,         // dst = src1

  // control flow
  TAC_LABEL,        // label:
  TAC_JUMP,         // goto label
  TAC_COND_JUMP,    // if src1 goto label

  // functions
  TAC_CALL,         // dst = call name(args...)
  TAC_RETURN,       // return src1

  // structs
  TAC_FIELD_LOAD,   // dst = src1.field
  TAC_FIELD_STORE,  // dst.field = src1

  // type conversion
  TAC_CAST,         // dst = (type)src1
} tac_op_t;


typedef struct {
  tac_op_t op;
  tac_operand_t dst;
  tac_operand_t src1;
  tac_operand_t src2;

  unsigned label;             // TAC_LABEL, TAC_JUMP, TAC_COND_JUMP

  char *call_name;            // TAC_CALL
  tac_operand_t *call_args;   // TAC_CALL  (arena-allocated)
  unsigned call_arg_count;    // TAC_CALL

  char *field_name;           // TAC_FIELD_LOAD, TAC_FIELD_STORE

  type_t cast_type;           // TAC_CAST
} tac_instr_t;


// ----------------------------------------------------------------
// Basic block
// ----------------------------------------------------------------

#define BB_MAX_SUCCS 2

typedef struct basic_block_t {
  unsigned id;

  tac_instr_t *instrs;       // arena-allocated
  unsigned instr_count;
  unsigned instr_capacity;

  struct basic_block_t *succs[BB_MAX_SUCCS];
  unsigned succ_count;
} basic_block_t;


// ----------------------------------------------------------------
// Control flow graph (one per function)
// ----------------------------------------------------------------

typedef struct {
  char *name;                 // function name (points into token array)
  type_t return_type;
  param_list_t params;

  basic_block_t **blocks;     // arena-allocated array of block pointers
  unsigned block_count;
  unsigned block_capacity;

  basic_block_t *entry;       // first block in the function

  unsigned next_temp;         // counter for generating temporaries
  unsigned next_label;        // counter for generating labels
} cfg_t;


// ----------------------------------------------------------------
// IR-level declarations (self-contained, no symtab needed by codegen)
// ----------------------------------------------------------------

typedef struct {
  char *name;
  type_t type;
  unsigned offset;            // byte offset within the struct
} ir_field_def_t;

typedef struct {
  char *name;
  ir_field_def_t *fields;     // arena-allocated
  unsigned field_count;
  unsigned total_size;        // total struct size in bytes
} ir_struct_def_t;

typedef enum {
  IR_INIT_NONE,
  IR_INIT_INT,
  IR_INIT_STR,
} ir_init_kind_t;

typedef struct {
  char *name;
  type_t type;
  ir_init_kind_t init_kind;
  union {
    long int_val;             // IR_INIT_INT
    char *str_val;            // IR_INIT_STR
  };
} ir_global_t;

typedef struct {
  char *name;
  type_t type;
  unsigned long addr;         // fixed hardware address
} ir_reg_def_t;


// ----------------------------------------------------------------
// External symbol (from forward declarations)
// ----------------------------------------------------------------

typedef struct {
  int is_function;
  char *name;
  type_t type;                  // return type (fn) or variable type (global)
  param_list_t params;          // only meaningful when is_function
} ir_extern_t;


// ----------------------------------------------------------------
// IR module (complete output of IR generation)
// ----------------------------------------------------------------

typedef struct {
  ir_struct_def_t *structs;   // arena-allocated
  unsigned struct_count;

  ir_global_t *globals;       // arena-allocated
  unsigned global_count;

  ir_reg_def_t *regs;         // arena-allocated
  unsigned reg_count;

  cfg_t *cfgs;                // arena-allocated, one per function
  unsigned cfg_count;

  ir_extern_t *externs;       // arena-allocated, from decl statements
  unsigned extern_count;
} ir_module_t;


// ----------------------------------------------------------------
// IR generator context
// ----------------------------------------------------------------

#define IR_ARENA_CHUNK_SIZE 4096

typedef struct {
  arena_t arena;
  ir_module_t module;

  unsigned struct_capacity;
  unsigned global_capacity;
  unsigned reg_capacity;
  unsigned cfg_capacity;
  unsigned extern_capacity;
} ir_gen_t;


// ----------------------------------------------------------------
// API
// ----------------------------------------------------------------

int  ir_gen_init(ir_gen_t *gen);
void ir_gen_free(ir_gen_t *gen);

int  ir_gen_run(ir_gen_t *gen, ast_t ast);

void ir_gen_print(ir_gen_t *gen);

int  ir_write(ir_gen_t *gen, const char *path);
int  ir_read(ir_gen_t *gen, const char *path);

#endif // __IR_H__
