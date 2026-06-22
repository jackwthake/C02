#include "ir.h"

#include <stdio.h>
#include <string.h>

/*
 * Binary serialization for ir_module_t.
 *
 * Every string in the IR (variable names, function names, field names,
 * struct_name inside type_t) is a borrowed pointer into the token array,
 * which doesn't exist when loading from a .o file. So we write string
 * contents as [u32 length][bytes] and allocate fresh copies from the
 * arena on read. A null/empty string is written as length 0.
 *
 * Format: magic, version, then each section length-prefixed.
 * Endianness is native (same-machine .o reuse only).
 */

#define IR_MAGIC   0x43303249  /* "C02I" */
#define IR_VERSION 2


// ----------------------------------------------------------------
// Write helpers
// ----------------------------------------------------------------

static int write_u32(FILE *f, unsigned v) {
  return fwrite(&v, 4, 1, f) == 1;
}

static int write_u64(FILE *f, unsigned long v) {
  return fwrite(&v, 8, 1, f) == 1;
}

static int write_i64(FILE *f, long v) {
  return fwrite(&v, 8, 1, f) == 1;
}

static int write_str(FILE *f, const char *s) {
  unsigned len = s ? (unsigned)strlen(s) : 0;
  if (!write_u32(f, len)) return 0;
  if (len > 0 && fwrite(s, 1, len, f) != len) return 0;
  return 1;
}

static int write_type(FILE *f, type_t t) {
  if (!write_u32(f, (unsigned)t.kind)) return 0;
  if (!write_u32(f, t.is_ptr)) return 0;
  if (!write_u32(f, t.ptr_depth)) return 0;
  if (t.kind == TYPE_STRUCT) {
    if (!write_str(f, t.struct_name)) return 0;
  }
  return 1;
}

static int write_operand(FILE *f, tac_operand_t op) {
  if (!write_u32(f, (unsigned)op.kind)) return 0;
  if (!write_type(f, op.type)) return 0;
  switch (op.kind) {
    case OPERAND_TEMP:      return write_u32(f, op.temp_id);
    case OPERAND_VAR:       return write_str(f, op.name);
    case OPERAND_CONST_INT: return write_i64(f, op.int_val);
    case OPERAND_CONST_STR: return write_str(f, op.str_val);
    case OPERAND_NONE:      return 1;
  }
  return 1;
}

static int write_instr(FILE *f, tac_instr_t *ins) {
  if (!write_u32(f, (unsigned)ins->op)) return 0;
  if (!write_operand(f, ins->dst)) return 0;
  if (!write_operand(f, ins->src1)) return 0;
  if (!write_operand(f, ins->src2)) return 0;
  if (!write_u32(f, ins->label)) return 0;
  if (!write_str(f, ins->call_name)) return 0;
  if (!write_u32(f, ins->call_arg_count)) return 0;
  for (unsigned i = 0; i < ins->call_arg_count; i++) {
    if (!write_operand(f, ins->call_args[i])) return 0;
  }
  if (!write_str(f, ins->field_name)) return 0;
  if (!write_type(f, ins->cast_type)) return 0;
  return 1;
}

static int write_block(FILE *f, basic_block_t *bb) {
  if (!write_u32(f, bb->id)) return 0;
  if (!write_u32(f, bb->instr_count)) return 0;
  for (unsigned i = 0; i < bb->instr_count; i++) {
    if (!write_instr(f, &bb->instrs[i])) return 0;
  }
  return 1;
}

static int write_cfg(FILE *f, cfg_t *cfg) {
  if (!write_str(f, cfg->name)) return 0;
  if (!write_type(f, cfg->return_type)) return 0;

  if (!write_u32(f, cfg->params.count)) return 0;
  for (unsigned i = 0; i < cfg->params.count; i++) {
    if (!write_type(f, cfg->params.items[i].type)) return 0;
    if (!write_str(f, cfg->params.items[i].name)) return 0;
  }

  if (!write_u32(f, cfg->block_count)) return 0;
  for (unsigned i = 0; i < cfg->block_count; i++) {
    if (!write_block(f, cfg->blocks[i])) return 0;
  }

  if (!write_u32(f, cfg->next_temp)) return 0;
  if (!write_u32(f, cfg->next_label)) return 0;
  return 1;
}

int ir_write(ir_gen_t *gen, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;

  ir_module_t *m = &gen->module;

  if (!write_u32(f, IR_MAGIC)) goto fail;
  if (!write_u32(f, IR_VERSION)) goto fail;

  // structs
  if (!write_u32(f, m->struct_count)) goto fail;
  for (unsigned i = 0; i < m->struct_count; i++) {
    ir_struct_def_t *s = &m->structs[i];
    if (!write_str(f, s->name)) goto fail;
    if (!write_u32(f, s->field_count)) goto fail;
    if (!write_u32(f, s->total_size)) goto fail;
    for (unsigned j = 0; j < s->field_count; j++) {
      if (!write_str(f, s->fields[j].name)) goto fail;
      if (!write_type(f, s->fields[j].type)) goto fail;
      if (!write_u32(f, s->fields[j].offset)) goto fail;
    }
  }

  // globals
  if (!write_u32(f, m->global_count)) goto fail;
  for (unsigned i = 0; i < m->global_count; i++) {
    ir_global_t *g = &m->globals[i];
    if (!write_str(f, g->name)) goto fail;
    if (!write_type(f, g->type)) goto fail;
    if (!write_u32(f, (unsigned)g->init_kind)) goto fail;
    switch (g->init_kind) {
      case IR_INIT_INT: if (!write_i64(f, g->int_val)) goto fail; break;
      case IR_INIT_STR: if (!write_str(f, g->str_val)) goto fail; break;
      case IR_INIT_NONE: break;
    }
  }

  // registers
  if (!write_u32(f, m->reg_count)) goto fail;
  for (unsigned i = 0; i < m->reg_count; i++) {
    if (!write_str(f, m->regs[i].name)) goto fail;
    if (!write_type(f, m->regs[i].type)) goto fail;
    if (!write_u64(f, m->regs[i].addr)) goto fail;
  }

  // cfgs
  if (!write_u32(f, m->cfg_count)) goto fail;
  for (unsigned i = 0; i < m->cfg_count; i++) {
    if (!write_cfg(f, &m->cfgs[i])) goto fail;
  }

  // externs
  if (!write_u32(f, m->extern_count)) goto fail;
  for (unsigned i = 0; i < m->extern_count; i++) {
    ir_extern_t *e = &m->externs[i];
    if (!write_u32(f, (unsigned)e->is_function)) goto fail;
    if (!write_str(f, e->name)) goto fail;
    if (!write_type(f, e->type)) goto fail;
    if (!write_u32(f, e->params.count)) goto fail;
    for (unsigned j = 0; j < e->params.count; j++) {
      if (!write_type(f, e->params.items[j].type)) goto fail;
      if (!write_str(f, e->params.items[j].name)) goto fail;
    }
  }

  fclose(f);
  return 1;

fail:
  fclose(f);
  return 0;
}


// ----------------------------------------------------------------
// Read helpers
// ----------------------------------------------------------------

static int read_u32(FILE *f, unsigned *v) {
  return fread(v, 4, 1, f) == 1;
}

static int read_u64(FILE *f, unsigned long *v) {
  return fread(v, 8, 1, f) == 1;
}

static int read_i64(FILE *f, long *v) {
  return fread(v, 8, 1, f) == 1;
}

// Reads a length-prefixed string into arena memory.
static int read_str(FILE *f, arena_t *a, char **out) {
  unsigned len;
  if (!read_u32(f, &len)) return 0;
  if (len == 0) { *out = NULL; return 1; }
  char *s = arena_alloc(a, len + 1);
  if (fread(s, 1, len, f) != len) return 0;
  s[len] = '\0';
  *out = s;
  return 1;
}

static int read_type(FILE *f, arena_t *a, type_t *t) {
  unsigned kind;
  if (!read_u32(f, &kind)) return 0;
  t->kind = (type_kind_t)kind;
  if (!read_u32(f, &t->is_ptr)) return 0;
  if (!read_u32(f, &t->ptr_depth)) return 0;
  if (t->kind == TYPE_STRUCT) {
    if (!read_str(f, a, &t->struct_name)) return 0;
  } else {
    t->struct_name = NULL;
  }
  return 1;
}

static int read_operand(FILE *f, arena_t *a, tac_operand_t *op) {
  unsigned kind;
  if (!read_u32(f, &kind)) return 0;
  op->kind = (tac_operand_kind_t)kind;
  if (!read_type(f, a, &op->type)) return 0;
  switch (op->kind) {
    case OPERAND_TEMP:      return read_u32(f, &op->temp_id);
    case OPERAND_VAR:       return read_str(f, a, &op->name);
    case OPERAND_CONST_INT: return read_i64(f, &op->int_val);
    case OPERAND_CONST_STR: return read_str(f, a, &op->str_val);
    case OPERAND_NONE:      return 1;
  }
  return 1;
}

static int read_instr(FILE *f, arena_t *a, tac_instr_t *ins) {
  unsigned op;
  if (!read_u32(f, &op)) return 0;
  ins->op = (tac_op_t)op;
  if (!read_operand(f, a, &ins->dst)) return 0;
  if (!read_operand(f, a, &ins->src1)) return 0;
  if (!read_operand(f, a, &ins->src2)) return 0;
  if (!read_u32(f, &ins->label)) return 0;
  if (!read_str(f, a, &ins->call_name)) return 0;
  if (!read_u32(f, &ins->call_arg_count)) return 0;
  if (ins->call_arg_count > 0) {
    ins->call_args = arena_alloc(a, sizeof(tac_operand_t) * ins->call_arg_count);
    for (unsigned i = 0; i < ins->call_arg_count; i++) {
      if (!read_operand(f, a, &ins->call_args[i])) return 0;
    }
  } else {
    ins->call_args = NULL;
  }
  if (!read_str(f, a, &ins->field_name)) return 0;
  if (!read_type(f, a, &ins->cast_type)) return 0;
  return 1;
}

static int read_block(FILE *f, arena_t *a, basic_block_t *bb) {
  if (!read_u32(f, &bb->id)) return 0;
  if (!read_u32(f, &bb->instr_count)) return 0;
  bb->instr_capacity = bb->instr_count;
  bb->instrs = arena_alloc(a, sizeof(tac_instr_t) * bb->instr_count);
  bb->succ_count = 0;
  for (unsigned i = 0; i < bb->instr_count; i++) {
    if (!read_instr(f, a, &bb->instrs[i])) return 0;
  }
  return 1;
}

static int read_cfg(FILE *f, arena_t *a, cfg_t *cfg) {
  if (!read_str(f, a, &cfg->name)) return 0;
  if (!read_type(f, a, &cfg->return_type)) return 0;

  if (!read_u32(f, &cfg->params.count)) return 0;
  if (cfg->params.count > 0) {
    cfg->params.items = arena_alloc(a, sizeof(param_t) * cfg->params.count);
    for (unsigned i = 0; i < cfg->params.count; i++) {
      if (!read_type(f, a, &cfg->params.items[i].type)) return 0;
      if (!read_str(f, a, &cfg->params.items[i].name)) return 0;
    }
  } else {
    cfg->params.items = NULL;
  }

  if (!read_u32(f, &cfg->block_count)) return 0;
  cfg->block_capacity = cfg->block_count;
  cfg->blocks = arena_alloc(a, sizeof(basic_block_t *) * cfg->block_count);
  for (unsigned i = 0; i < cfg->block_count; i++) {
    cfg->blocks[i] = ARENA_ALLOC(a, basic_block_t);
    if (!read_block(f, a, cfg->blocks[i])) return 0;
  }
  cfg->entry = cfg->block_count > 0 ? cfg->blocks[0] : NULL;

  if (!read_u32(f, &cfg->next_temp)) return 0;
  if (!read_u32(f, &cfg->next_label)) return 0;
  return 1;
}

int ir_read(ir_gen_t *gen, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;

  if (!ir_gen_init(gen)) { fclose(f); return 0; }
  arena_t *a = &gen->arena;
  ir_module_t *m = &gen->module;

  unsigned magic, version;
  if (!read_u32(f, &magic) || magic != IR_MAGIC) goto fail;
  if (!read_u32(f, &version) || version != IR_VERSION) goto fail;

  // structs
  if (!read_u32(f, &m->struct_count)) goto fail;
  m->structs = arena_alloc(a, sizeof(ir_struct_def_t) * m->struct_count);
  gen->struct_capacity = m->struct_count;
  for (unsigned i = 0; i < m->struct_count; i++) {
    ir_struct_def_t *s = &m->structs[i];
    if (!read_str(f, a, &s->name)) goto fail;
    if (!read_u32(f, &s->field_count)) goto fail;
    if (!read_u32(f, &s->total_size)) goto fail;
    s->fields = arena_alloc(a, sizeof(ir_field_def_t) * s->field_count);
    for (unsigned j = 0; j < s->field_count; j++) {
      if (!read_str(f, a, &s->fields[j].name)) goto fail;
      if (!read_type(f, a, &s->fields[j].type)) goto fail;
      if (!read_u32(f, &s->fields[j].offset)) goto fail;
    }
  }

  // globals
  if (!read_u32(f, &m->global_count)) goto fail;
  m->globals = arena_alloc(a, sizeof(ir_global_t) * m->global_count);
  gen->global_capacity = m->global_count;
  for (unsigned i = 0; i < m->global_count; i++) {
    ir_global_t *g = &m->globals[i];
    if (!read_str(f, a, &g->name)) goto fail;
    if (!read_type(f, a, &g->type)) goto fail;
    unsigned ik;
    if (!read_u32(f, &ik)) goto fail;
    g->init_kind = (ir_init_kind_t)ik;
    switch (g->init_kind) {
      case IR_INIT_INT: if (!read_i64(f, &g->int_val)) goto fail; break;
      case IR_INIT_STR: if (!read_str(f, a, &g->str_val)) goto fail; break;
      case IR_INIT_NONE: break;
    }
  }

  // registers
  if (!read_u32(f, &m->reg_count)) goto fail;
  m->regs = arena_alloc(a, sizeof(ir_reg_def_t) * m->reg_count);
  gen->reg_capacity = m->reg_count;
  for (unsigned i = 0; i < m->reg_count; i++) {
    if (!read_str(f, a, &m->regs[i].name)) goto fail;
    if (!read_type(f, a, &m->regs[i].type)) goto fail;
    if (!read_u64(f, &m->regs[i].addr)) goto fail;
  }

  // cfgs
  if (!read_u32(f, &m->cfg_count)) goto fail;
  m->cfgs = arena_alloc(a, sizeof(cfg_t) * m->cfg_count);
  gen->cfg_capacity = m->cfg_count;
  for (unsigned i = 0; i < m->cfg_count; i++) {
    if (!read_cfg(f, a, &m->cfgs[i])) goto fail;
  }

  // externs
  if (!read_u32(f, &m->extern_count)) goto fail;
  m->externs = arena_alloc(a, sizeof(ir_extern_t) * m->extern_count);
  gen->extern_capacity = m->extern_count;
  for (unsigned i = 0; i < m->extern_count; i++) {
    ir_extern_t *e = &m->externs[i];
    unsigned is_fn;
    if (!read_u32(f, &is_fn)) goto fail;
    e->is_function = (int)is_fn;
    if (!read_str(f, a, &e->name)) goto fail;
    if (!read_type(f, a, &e->type)) goto fail;
    if (!read_u32(f, &e->params.count)) goto fail;
    if (e->params.count > 0) {
      e->params.items = arena_alloc(a, sizeof(param_t) * e->params.count);
      for (unsigned j = 0; j < e->params.count; j++) {
        if (!read_type(f, a, &e->params.items[j].type)) goto fail;
        if (!read_str(f, a, &e->params.items[j].name)) goto fail;
      }
    } else {
      e->params.items = NULL;
    }
  }

  fclose(f);
  return 1;

fail:
  fclose(f);
  ir_gen_free(gen);
  return 0;
}
