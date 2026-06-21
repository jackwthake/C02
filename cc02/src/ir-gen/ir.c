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
        g->int_val = node->global_var.initialiser->number;
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

static void collect_declarations(ir_gen_t *gen, ast_t ast) {
  for (unsigned i = 0; i < ast->program.count; i++) {
    node_t *node = ast->program.items[i];
    switch (node->kind) {
      case NODE_REG_DECL:    collect_reg(gen, node);    break;
      case NODE_GLOBAL_VAR:  collect_global(gen, node); break;
      case NODE_STRUCT_DECL: collect_struct(gen, node); break;
      default: break;
    }
  }
}


int ir_gen_run(ir_gen_t *gen, ast_t ast, analyzer_t *analyzer) {
  (void)analyzer;

  collect_declarations(gen, ast);

  return 1;
}
