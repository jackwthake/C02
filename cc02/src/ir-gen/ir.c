#include "ir.h"

#include <string.h>


#define INITIAL_CAPACITY 8


// ----------------------------------------------------------------
// Growable array helpers
// ----------------------------------------------------------------
// Each module array (structs, globals, regs, cfgs) can outgrow its
// initial allocation. These helpers double the backing array when
// full - same pattern as analyzer_scope_push().

static void grow_regs(ir_gen_t *gen) {
  unsigned new_cap = gen->reg_capacity * 2;
  ir_reg_def_t *grown = arena_alloc(&gen->arena, sizeof(ir_reg_def_t) * new_cap);
  memcpy(grown, gen->module.regs, sizeof(ir_reg_def_t) * gen->module.reg_count);
  gen->module.regs = grown;
  gen->reg_capacity = new_cap;
}


static void grow_globals(ir_gen_t *gen) {
  unsigned new_cap = gen->global_capacity * 2;
  ir_global_t *grown = arena_alloc(&gen->arena, sizeof(ir_global_t) * new_cap);
  memcpy(grown, gen->module.globals, sizeof(ir_global_t) * gen->module.global_count);
  gen->module.globals = grown;
  gen->global_capacity = new_cap;
}


static void grow_structs(ir_gen_t *gen) {
  unsigned new_cap = gen->struct_capacity * 2;
  ir_struct_def_t *grown = arena_alloc(&gen->arena, sizeof(ir_struct_def_t) * new_cap);
  memcpy(grown, gen->module.structs, sizeof(ir_struct_def_t) * gen->module.struct_count);
  gen->module.structs = grown;
  gen->struct_capacity = new_cap;
}


static void grow_externs(ir_gen_t *gen) {
  unsigned new_cap = gen->extern_capacity * 2;
  ir_extern_t *grown = arena_alloc(&gen->arena, sizeof(ir_extern_t) * new_cap);
  memcpy(grown, gen->module.externs, sizeof(ir_extern_t) * gen->module.extern_count);
  gen->module.externs = grown;
  gen->extern_capacity = new_cap;
}


int ir_gen_init(ir_gen_t *gen) {
  if (!arena_init(&gen->arena, IR_ARENA_CHUNK_SIZE))
    return 0;

  gen->struct_capacity = INITIAL_CAPACITY;
  gen->global_capacity = INITIAL_CAPACITY;
  gen->reg_capacity    = INITIAL_CAPACITY;
  gen->cfg_capacity    = INITIAL_CAPACITY;

  gen->module.structs = arena_alloc(&gen->arena, sizeof(ir_struct_def_t) * gen->struct_capacity);
  gen->module.struct_count = 0;

  gen->module.globals = arena_alloc(&gen->arena, sizeof(ir_global_t) * gen->global_capacity);
  gen->module.global_count = 0;

  gen->module.regs = arena_alloc(&gen->arena, sizeof(ir_reg_def_t) * gen->reg_capacity);
  gen->module.reg_count = 0;

  gen->module.cfgs = arena_alloc(&gen->arena, sizeof(cfg_t) * gen->cfg_capacity);
  gen->module.cfg_count = 0;

  gen->extern_capacity = INITIAL_CAPACITY;
  gen->module.externs = arena_alloc(&gen->arena, sizeof(ir_extern_t) * gen->extern_capacity);
  gen->module.extern_count = 0;

  return 1;
}


void ir_gen_free(ir_gen_t *gen) {
  if (gen) {
    arena_free(&gen->arena);
  }
}


// ----------------------------------------------------------------
// Type sizing
// ----------------------------------------------------------------
// Returns the size in bytes of a C02 type. Pointers are 2 bytes
// (16-bit address space). Struct sizes come from the module's
// struct definitions (looked up by name).

static unsigned type_size(ir_gen_t *gen, type_t type) {
  if (type.is_ptr) return 2;

  switch (type.kind) {
    case TYPE_U8:  case TYPE_I8:  return 1;
    case TYPE_U16: case TYPE_I16: return 2;
    case TYPE_VOID: return 0;
    case TYPE_STRUCT:
      for (unsigned i = 0; i < gen->module.struct_count; i++) {
        if (strcmp(gen->module.structs[i].name, type.struct_name) == 0)
          return gen->module.structs[i].total_size;
      }
      return 0;
    case TYPE_INVALID: return 0;
  }
  return 0;
}


// ----------------------------------------------------------------
// Pass 1: collect top-level declarations
// ----------------------------------------------------------------
// Walks the NODE_PROGRAM list once and populates module.regs,
// module.globals, and module.structs. Functions are skipped here -
// they get their own CFGs in pass 2.

static void collect_reg(ir_gen_t *gen, node_t *node) {
  if (gen->module.reg_count == gen->reg_capacity)
    grow_regs(gen);

  ir_reg_def_t *r = &gen->module.regs[gen->module.reg_count++];
  r->name = node->reg_decl.name;
  r->type = node->reg_decl.type;
  r->addr = node->reg_decl.addr;
}


static void collect_global(ir_gen_t *gen, node_t *node) {
  if (gen->module.global_count == gen->global_capacity)
    grow_globals(gen);

  ir_global_t *g = &gen->module.globals[gen->module.global_count++];
  g->name = node->global_var.name;
  g->type = node->global_var.type;
  g->init_kind = IR_INIT_NONE;

  if (node->global_var.initialiser) {
    switch (node->global_var.initialiser->kind) {
      case NODE_NUMBER:
        g->init_kind = IR_INIT_INT;
        g->int_val = node->global_var.initialiser->number.value;
        break;
      case NODE_STRING:
        g->init_kind = IR_INIT_STR;
        g->str_val = node->global_var.initialiser->value;
        break;
      default:
        break;
    }
  }
}


static void collect_struct(ir_gen_t *gen, node_t *node) {
  if (gen->module.struct_count == gen->struct_capacity)
    grow_structs(gen);

  ir_struct_def_t *s = &gen->module.structs[gen->module.struct_count++];
  s->name = node->struct_decl.name;
  s->field_count = node->struct_decl.fields.count;
  s->fields = arena_alloc(&gen->arena, sizeof(ir_field_def_t) * s->field_count);

  // lay out fields sequentially with no padding (this is a simple
  // 8/16-bit target where alignment doesn't matter)
  unsigned offset = 0;
  for (unsigned i = 0; i < s->field_count; i++) {
    s->fields[i].name = node->struct_decl.fields.items[i].name;
    s->fields[i].type = node->struct_decl.fields.items[i].type;
    s->fields[i].offset = offset;
    offset += type_size(gen, s->fields[i].type);
  }
  s->total_size = offset;
}


static void grow_cfgs(ir_gen_t *gen) {
  unsigned new_cap = gen->cfg_capacity * 2;
  cfg_t *grown = arena_alloc(&gen->arena, sizeof(cfg_t) * new_cap);
  memcpy(grown, gen->module.cfgs, sizeof(cfg_t) * gen->module.cfg_count);
  gen->module.cfgs = grown;
  gen->cfg_capacity = new_cap;
}


static void collect_extern(ir_gen_t *gen, node_t *node) {
  if (gen->module.extern_count == gen->extern_capacity)
    grow_externs(gen);

  ir_extern_t *e = &gen->module.externs[gen->module.extern_count++];
  e->is_function = node->fwd_decl.is_function;
  e->name = node->fwd_decl.name;
  e->type = node->fwd_decl.type;
  e->params = node->fwd_decl.params;
}


static void collect_declarations(ir_gen_t *gen, ast_t ast) {
  for (unsigned i = 0; i < ast->program.count; i++) {
    node_t *node = ast->program.items[i];
    switch (node->kind) {
      case NODE_REG_DECL:    collect_reg(gen, node);    break;
      case NODE_GLOBAL_VAR:  collect_global(gen, node); break;
      case NODE_STRUCT_DECL: collect_struct(gen, node); break;
      case NODE_FWD_DECL:    collect_extern(gen, node); break;
      default: break;
    }
  }
}


// ----------------------------------------------------------------
// op_t -> tac_op_t mapping
// ----------------------------------------------------------------

static tac_op_t op_to_tac(op_t op) {
  switch (op) {
    case OP_PLUS:        return TAC_ADD;
    case OP_MINUS:       return TAC_SUB;
    case OP_MULTIPLY:    return TAC_MUL;
    case OP_DIVIDE:      return TAC_DIV;
    case OP_MODULUS:     return TAC_MOD;
    case OP_LT:          return TAC_LT;
    case OP_GT:          return TAC_GT;
    case OP_LTE:         return TAC_LTE;
    case OP_GTE:         return TAC_GTE;
    case OP_EQUALSEQUALS:return TAC_EQ;
    case OP_BANGEQUALS:  return TAC_NEQ;
    case OP_AND:         return TAC_AND;
    case OP_OR:          return TAC_OR;
    case OP_LEFT_SHIFT:  return TAC_SHL;
    case OP_RIGHT_SHIFT: return TAC_SHR;
    case OP_BAND:        return TAC_BAND;
    case OP_BXOR:        return TAC_BXOR;
    case OP_BOR:         return TAC_BOR;
    case OP_NEGATE:      return TAC_NEG;
    case OP_BANG:        return TAC_NOT;
    case OP_BNOT:        return TAC_BNOT;
    case OP_ADDRESSOF:   return TAC_ADDR_OF;
    default:             return TAC_ADD;
  }
}


// ----------------------------------------------------------------
// Pass 2: lower functions to TAC
// ----------------------------------------------------------------
// Each function gets a cfg_t. The AST is recursively walked and
// flattened into a linear sequence of TAC instructions in a single
// block. Control flow (if/while/for) is expressed with labels and
// jumps — the instruction list stays flat, and branches skip over
// or loop back to labelled positions.
//
// lower_expr returns a tac_operand_t (the value the expression
// produced — a temp, variable, or constant). lower_stmt is void
// and just emits instructions.

#define INSTR_INITIAL_CAPACITY 32

// Allocates a fresh basic block with room for instructions.
static basic_block_t *alloc_block(ir_gen_t *gen, cfg_t *cfg) {
  basic_block_t *bb = ARENA_ALLOC(&gen->arena, basic_block_t);
  bb->id = cfg->block_count;
  bb->instrs = arena_alloc(&gen->arena, sizeof(tac_instr_t) * INSTR_INITIAL_CAPACITY);
  bb->instr_count = 0;
  bb->instr_capacity = INSTR_INITIAL_CAPACITY;
  bb->succ_count = 0;

  // add to cfg's block list
  if (cfg->block_count == cfg->block_capacity) {
    unsigned new_cap = cfg->block_capacity * 2;
    basic_block_t **grown = arena_alloc(&gen->arena, sizeof(basic_block_t *) * new_cap);
    memcpy(grown, cfg->blocks, sizeof(basic_block_t *) * cfg->block_count);
    cfg->blocks = grown;
    cfg->block_capacity = new_cap;
  }
  cfg->blocks[cfg->block_count++] = bb;
  return bb;
}


// Appends one instruction to the current (last) block in the CFG.
static void emit(ir_gen_t *gen, cfg_t *cfg, tac_instr_t instr) {
  basic_block_t *bb = cfg->blocks[cfg->block_count - 1];
  if (bb->instr_count == bb->instr_capacity) {
    unsigned new_cap = bb->instr_capacity * 2;
    tac_instr_t *grown = arena_alloc(&gen->arena, sizeof(tac_instr_t) * new_cap);
    memcpy(grown, bb->instrs, sizeof(tac_instr_t) * bb->instr_count);
    bb->instrs = grown;
    bb->instr_capacity = new_cap;
  }
  bb->instrs[bb->instr_count++] = instr;
}


// Returns a fresh temporary operand (t0, t1, ...) with the given type.
static tac_operand_t new_temp(cfg_t *cfg, type_t type) {
  return (tac_operand_t){
    .kind = OPERAND_TEMP,
    .type = type,
    .temp_id = cfg->next_temp++,
  };
}


// Returns the next label ID (L0, L1, ...).
static unsigned new_label(cfg_t *cfg) {
  return cfg->next_label++;
}


// Lowers an expression node into TAC, returning the operand
// that holds the result. Built up case by case.
static tac_operand_t lower_expr(ir_gen_t *gen, cfg_t *cfg, node_t *node) {
  switch (node->kind) {
    case NODE_NUMBER:
      return (tac_operand_t){
        .kind = OPERAND_CONST_INT,
        .type = node->number.resolved_type,
        .int_val = node->number.value
      };
    
    case NODE_STRING:
      return (tac_operand_t) {
        .kind = OPERAND_CONST_STR,
        .type = { .kind = TYPE_U8, .is_ptr = 1, .ptr_depth = 1, .struct_name = "" },
        .str_val = node->value
      };
    
    case NODE_IDENTIFIER: {
      for (unsigned i = 0; i < gen->module.reg_count; i++) {
        if (strcmp(gen->module.regs[i].name, node->identifier.name) == 0) {
          ir_reg_def_t *reg = &gen->module.regs[i];
          tac_operand_t addr = {
            .kind = OPERAND_CONST_INT,
            .type = { .kind = TYPE_U16 },
            .int_val = (long)reg->addr,
          };
          tac_operand_t dst = new_temp(cfg, reg->type);
          emit(gen, cfg, (tac_instr_t){ .op = TAC_LOAD, .dst = dst, .src1 = addr });
          return dst;
        }
      }
      return (tac_operand_t){
        .kind = OPERAND_VAR,
        .type = node->identifier.resolved_type,
        .name = node->identifier.name,
      };
    }
    
    case NODE_BINOP: {
      if (node->binop.op == OP_AND) {
        tac_operand_t result = new_temp(cfg, (type_t){ .kind = TYPE_U8 });
        unsigned false_label = new_label(cfg);
        unsigned end_label = new_label(cfg);

        tac_operand_t left = lower_expr(gen, cfg, node->binop.left);
        tac_operand_t neg = new_temp(cfg, (type_t){ .kind = TYPE_U8 });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_NOT, .dst = neg, .src1 = left });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COND_JUMP, .src1 = neg, .label = false_label });

        tac_operand_t right = lower_expr(gen, cfg, node->binop.right);
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COPY, .dst = result, .src1 = right });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_JUMP, .label = end_label });

        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = false_label });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COPY, .dst = result,
          .src1 = { .kind = OPERAND_CONST_INT, .type = { .kind = TYPE_U8 }, .int_val = 0 } });

        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = end_label });
        return result;
      }

      if (node->binop.op == OP_OR) {
        tac_operand_t result = new_temp(cfg, (type_t){ .kind = TYPE_U8 });
        unsigned true_label = new_label(cfg);
        unsigned end_label = new_label(cfg);

        tac_operand_t left = lower_expr(gen, cfg, node->binop.left);
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COND_JUMP, .src1 = left, .label = true_label });

        tac_operand_t right = lower_expr(gen, cfg, node->binop.right);
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COPY, .dst = result, .src1 = right });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_JUMP, .label = end_label });

        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = true_label });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COPY, .dst = result,
          .src1 = { .kind = OPERAND_CONST_INT, .type = { .kind = TYPE_U8 }, .int_val = 1 } });

        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = end_label });
        return result;
      }

      tac_op_t op = op_to_tac(node->binop.op);

      tac_operand_t left  = lower_expr(gen, cfg, node->binop.left);
      tac_operand_t right = lower_expr(gen, cfg, node->binop.right);

      int is_cmp = op == TAC_LT || op == TAC_LTE ||
                   op == TAC_GT || op == TAC_GTE ||
                   op == TAC_EQ || op == TAC_NEQ;

      type_t type = is_cmp ? (type_t){ .kind = TYPE_U8 } : left.type;
      tac_operand_t dst = new_temp(cfg, type);

      emit(gen, cfg, (tac_instr_t){ .op = op, .dst = dst, .src1 = left, .src2 = right });
      return dst;
    }

    case NODE_UNARY: {
      if (node->unary.op == OP_INCREMENT || node->unary.op == OP_DECREMENT) {
        tac_op_t op = node->unary.op == OP_INCREMENT ? TAC_INC : TAC_DEC;

        if (node->unary.operand->kind == NODE_FIELD_ACCESS) {
          node_t *fa = node->unary.operand;
          tac_operand_t base = lower_expr(gen, cfg, fa->field_access.base);
          tac_operand_t tmp = new_temp(cfg, fa->field_access.resolved_type);
          emit(gen, cfg, (tac_instr_t){
            .op = TAC_FIELD_LOAD, .dst = tmp, .src1 = base,
            .field_name = fa->field_access.field,
          });
          emit(gen, cfg, (tac_instr_t){ .op = op, .dst = tmp });
          emit(gen, cfg, (tac_instr_t){
            .op = TAC_FIELD_STORE, .dst = base, .src1 = tmp,
            .field_name = fa->field_access.field,
          });
          return tmp;
        }

        tac_operand_t operand = lower_expr(gen, cfg, node->unary.operand);
        emit(gen, cfg, (tac_instr_t){ .op = op, .dst = operand });
        return operand;
      }

      tac_op_t op = op_to_tac(node->unary.op);
      tac_operand_t operand = lower_expr(gen, cfg, node->unary.operand);

      type_t result_type = operand.type;
      if (op == TAC_NOT) {
        result_type = (type_t){ .kind = TYPE_U8 };
      } else if (op == TAC_ADDR_OF) {
        result_type.is_ptr = 1;
        result_type.ptr_depth++;
      }

      tac_operand_t dst = new_temp(cfg, result_type);
      emit(gen, cfg, (tac_instr_t){ .op = op, .dst = dst, .src1 = operand });
      return dst;
    }

    case NODE_CAST: {
      tac_operand_t src = lower_expr(gen, cfg, node->cast.operand);
      tac_operand_t dst = new_temp(cfg, node->cast.cast_type);
      emit(gen, cfg, (tac_instr_t){
        .op = TAC_CAST,
        .dst = dst,
        .src1 = src,
        .cast_type = node->cast.cast_type,
      });
      return dst;
    }

    case NODE_DEREF: {
      tac_operand_t src = lower_expr(gen, cfg, node->deref_target);
      type_t result_type = src.type;
      result_type.ptr_depth--;
      result_type.is_ptr = result_type.ptr_depth > 0;

      tac_operand_t dst = new_temp(cfg, result_type);
      emit(gen, cfg, (tac_instr_t){ .op = TAC_LOAD, .dst = dst, .src1 = src });
      return dst;
    }


    case NODE_CALL: {
      unsigned arg_count = node->call.args.count;
      tac_operand_t *args = NULL;
      if (arg_count > 0) {
        args = arena_alloc(&gen->arena, sizeof(tac_operand_t) * arg_count);
        for (unsigned i = 0; i < arg_count; i++) {
          args[i] = lower_expr(gen, cfg, node->call.args.items[i]);
        }
      }

      tac_operand_t dst = new_temp(cfg, node->call.resolved_return_type);
      emit(gen, cfg, (tac_instr_t){
        .op = TAC_CALL,
        .dst = dst,
        .call_name = node->call.name,
        .call_args = args,
        .call_arg_count = arg_count,
      });
      return dst;
    }

    case NODE_FIELD_ACCESS: {
      tac_operand_t base = lower_expr(gen, cfg, node->field_access.base);
      tac_operand_t dst = new_temp(cfg, node->field_access.resolved_type);
      emit(gen, cfg, (tac_instr_t){
        .op = TAC_FIELD_LOAD,
        .dst = dst,
        .src1 = base,
        .field_name = node->field_access.field,
      });
      return dst;
    }

    case NODE_STRUCT_INIT: {
      type_t struct_type = {
        .kind = TYPE_STRUCT,
        .struct_name = node->struct_init.struct_name,
      };
      tac_operand_t dst = new_temp(cfg, struct_type);

      for (unsigned i = 0; i < node->struct_init.inits.count; i++) {
        field_init_t *init = &node->struct_init.inits.items[i];
        tac_operand_t val = lower_expr(gen, cfg, init->value);
        emit(gen, cfg, (tac_instr_t){
          .op = TAC_FIELD_STORE,
          .dst = dst,
          .src1 = val,
          .field_name = init->field_name,
        });
      }
      return dst;
    }

    default:
      return new_temp(cfg, (type_t){ .kind = TYPE_VOID });
  }
}


static void lower_block(ir_gen_t *gen, cfg_t *cfg, node_t *block);

// Dispatches on node kind — to be filled in case by case.
static void lower_stmt(ir_gen_t *gen, cfg_t *cfg, node_t *node) {
  switch (node->kind) {
    case NODE_RETURN: {
      emit(gen, cfg, (tac_instr_t){ .op = TAC_RETURN,
        .src1 = node->return_val ? lower_expr(gen, cfg, node->return_val)
                                 : (tac_operand_t){ .kind = OPERAND_NONE },
      });
      break;
    }

    case NODE_VAR_DECL: {
      if (!node->var_decl.initialiser) break; // no initializer -> nothing to emit

      tac_operand_t initter = lower_expr(gen, cfg, node->var_decl.initialiser);
      emit(gen, cfg, (tac_instr_t) {
        .op = TAC_COPY,
        .dst = { .kind = OPERAND_VAR, .type = node->var_decl.type, .name = node->var_decl.name },
        .src1 = initter,
      });

      break;
    }

    // The right side is always lowered as an expression. The left side
    // (target) determines what kind of write to emit: a plain copy for
    // variables, a TAC_STORE for pointer derefs and hardware registers,
    // or a TAC_FIELD_STORE for struct fields.
    case NODE_ASSIGN: {
      tac_operand_t val = lower_expr(gen, cfg, node->assign.value);
      node_t *target = node->assign.target;

      switch (target->kind) {
        case NODE_IDENTIFIER: {
          // check if it's a register — emit a store to the fixed address
          ir_reg_def_t *reg = NULL;
          for (unsigned i = 0; i < gen->module.reg_count; i++) {
            if (strcmp(gen->module.regs[i].name, target->identifier.name) == 0) {
              reg = &gen->module.regs[i];
              break;
            }
          }

          if (reg) {
            tac_operand_t addr = {
              .kind = OPERAND_CONST_INT,
              .type = { .kind = TYPE_U16 },
              .int_val = (long)reg->addr,
            };
            emit(gen, cfg, (tac_instr_t){ .op = TAC_STORE, .dst = addr, .src1 = val });
          } else {
            tac_operand_t var = {
              .kind = OPERAND_VAR,
              .type = target->identifier.resolved_type,
              .name = target->identifier.name,
            };
            emit(gen, cfg, (tac_instr_t){ .op = TAC_COPY, .dst = var, .src1 = val });
          }
          break;
        }

        case NODE_DEREF: {
          tac_operand_t ptr = lower_expr(gen, cfg, target->deref_target);
          emit(gen, cfg, (tac_instr_t){ .op = TAC_STORE, .dst = ptr, .src1 = val });
          break;
        }

        case NODE_FIELD_ACCESS: {
          tac_operand_t base = lower_expr(gen, cfg, target->field_access.base);
          emit(gen, cfg, (tac_instr_t){
            .op = TAC_FIELD_STORE,
            .dst = base,
            .src1 = val,
            .field_name = target->field_access.field,
          });
          break;
        }

        default:
          break;
      }
      break;
    }

    // TAC_COND_JUMP jumps when its operand is *true* (nonzero), but we
    // want to skip the then-body when the condition is *false*. So we
    // negate the condition and jump on the negated value — if the
    // original condition was false, the negated value is true, and the
    // jump fires, skipping the body. If the condition was true, the
    // negated value is false, the jump doesn't fire, and execution
    // falls through into the then-body.
    case NODE_IF: {
      unsigned block_count = node->if_stmt.blocks.count;
      int has_else = block_count > 1;

      tac_operand_t cond = lower_expr(gen, cfg, node->if_stmt.cond);
      tac_operand_t neg = new_temp(cfg, (type_t){ .kind = TYPE_U8 });
      emit(gen, cfg, (tac_instr_t){ .op = TAC_NOT, .dst = neg, .src1 = cond });

      unsigned else_label = new_label(cfg);
      unsigned end_label = has_else ? new_label(cfg) : else_label;

      emit(gen, cfg, (tac_instr_t){ .op = TAC_COND_JUMP, .src1 = neg, .label = else_label });

      lower_block(gen, cfg, node->if_stmt.blocks.items[0]);

      if (has_else) {
        emit(gen, cfg, (tac_instr_t){ .op = TAC_JUMP, .label = end_label });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = else_label });

        for (unsigned i = 1; i < block_count; i++) {
          node_t *branch = node->if_stmt.blocks.items[i];
          if (branch->kind == NODE_IF) {
            lower_stmt(gen, cfg, branch);
          } else {
            lower_block(gen, cfg, branch);
          }
        }

        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = end_label });
      } else {
        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = else_label });
      }
      break;
    }

    case NODE_WHILE: {
      unsigned cond_label = new_label(cfg); // while
      unsigned end_label = new_label(cfg);

      // point to jump back to loop condition check
      emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = cond_label });

      // evaluate condition
      tac_operand_t cond = lower_expr(gen, cfg, node->while_stmt.cond);
      tac_operand_t neg = new_temp(cfg, (type_t){ .kind = TYPE_U8 });

      // if !condition -> jump to end
      emit(gen, cfg, (tac_instr_t){ .op = TAC_NOT, .dst = neg, .src1 = cond });
      emit(gen, cfg, (tac_instr_t){ .op = TAC_COND_JUMP, .src1 = neg, .label = end_label });

      // otherwise fall through to actual while block
      if (node->while_stmt.body)
        lower_block(gen, cfg, node->while_stmt.body);

      // jump back to re-evaluate conditional
      emit(gen, cfg, (tac_instr_t){ .op = TAC_JUMP, .label = cond_label });

      // where we jump to when cond is false
      emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = end_label });
      break;
    }

    case NODE_FOR: {
      unsigned cond_label = new_label(cfg); // for (<initter>; <condition>; <incrementer>) <body>
      unsigned end_label = new_label(cfg);

      // initter
      if (node->for_stmt.initialiser)
        lower_stmt(gen, cfg, node->for_stmt.initialiser);

      // point to jump back to loop condition check
      emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = cond_label });

       // evaluate condition
      if (node->for_stmt.cond) {
        tac_operand_t cond = lower_expr(gen, cfg, node->for_stmt.cond);
        tac_operand_t neg = new_temp(cfg, (type_t){ .kind = TYPE_U8 });

        // if !condition -> jump to end
        emit(gen, cfg, (tac_instr_t){ .op = TAC_NOT, .dst = neg, .src1 = cond });
        emit(gen, cfg, (tac_instr_t){ .op = TAC_COND_JUMP, .src1 = neg, .label = end_label });
      }

      // otherwise fall through to actual for block
      if (node->for_stmt.body)
        lower_block(gen, cfg, node->for_stmt.body);

      // after body runs, do incrementer
      if (node->for_stmt.incrementer)
        lower_expr(gen, cfg, node->for_stmt.incrementer);

      // jump back to re-evaluate conditional
      emit(gen, cfg, (tac_instr_t){ .op = TAC_JUMP, .label = cond_label });

      // where we jump to when cond is false, don't need it if no conditional
      if (node->for_stmt.cond)
        emit(gen, cfg, (tac_instr_t){ .op = TAC_LABEL, .label = end_label });
      break;
    }

    case NODE_BLOCK:
      lower_block(gen, cfg, node);
      break;

    default:
      // expression statements (bare function calls like lcd_init())
      lower_expr(gen, cfg, node);
      break;
  }
}


// Lowers a NODE_BLOCK — walks each statement in order.
static void lower_block(ir_gen_t *gen, cfg_t *cfg, node_t *block) {
  for (unsigned i = 0; i < block->block.count; i++) {
    lower_stmt(gen, cfg, block->block.items[i]);
  }
}


// Creates a CFG for one function and lowers its body.
static void lower_function(ir_gen_t *gen, node_t *node) {
  if (gen->module.cfg_count == gen->cfg_capacity)
    grow_cfgs(gen);

  cfg_t *cfg = &gen->module.cfgs[gen->module.cfg_count++];
  cfg->name = node->function.name;
  cfg->return_type = node->function.return_type;
  cfg->params = node->function.params;
  cfg->next_temp = 0;
  cfg->next_label = 0;
  cfg->block_count = 0;
  cfg->block_capacity = INITIAL_CAPACITY;
  cfg->blocks = arena_alloc(&gen->arena, sizeof(basic_block_t *) * cfg->block_capacity);

  // create the entry block — all instructions go here for now
  cfg->entry = alloc_block(gen, cfg);

  // lower the function body
  if (node->function.body) {
    lower_block(gen, cfg, node->function.body);
  }

  basic_block_t *entry = cfg->entry;
  if (entry->instr_count == 0 ||
      entry->instrs[entry->instr_count - 1].op != TAC_RETURN) {
    emit(gen, cfg, (tac_instr_t){ .op = TAC_RETURN });
  }
}


int ir_gen_run(ir_gen_t *gen, ast_t ast) {
  collect_declarations(gen, ast);

  // pass 2: lower each function into a CFG
  for (unsigned i = 0; i < ast->program.count; i++) {
    node_t *node = ast->program.items[i];
    if (node->kind == NODE_FUNCTION) {
      lower_function(gen, node);
    }
  }

  return 1;
}
