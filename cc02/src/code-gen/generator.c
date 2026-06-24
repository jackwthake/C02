#include "generator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------
// Memory map
// ----------------------------------------------------------------

#define ROM_SIZE    0x8000
#define RAM_START   0x0200 // 6502 hardware stack occupies 0x0100 - 0x01FF
#define ROM_START   0x8000

#define FP          0x00
#define RET         0x02
#define REG_START   0x04
#define PARAM_START 0xEF


// ----------------------------------------------------------------
// Zero-page operand map
// ----------------------------------------------------------------

static unsigned codegen_type_size(type_t type) {
  if (type.is_ptr) return 2;
  switch (type.kind) {
    case TYPE_U8:  case TYPE_I8:  return 1;
    case TYPE_U16: case TYPE_I16: return 2;
    default: return 1;
  }
}


static uint8_t zp_map_lookup(zp_map_t *map, tac_operand_t *op) {
  for (unsigned i = 0; i < map->count; i++) {
    zp_entry_t *e = &map->entries[i];
    if (e->kind != op->kind) continue;
    if (e->kind == OPERAND_VAR && strcmp(e->name, op->name) == 0) return e->zp_addr;
    if (e->kind == OPERAND_TEMP && e->temp_id == op->temp_id)    return e->zp_addr;
  }
  return 0;
}


static void zp_map_add(zp_map_t *map, tac_operand_kind_t kind,
                        char *name, unsigned temp_id, type_t type) {
  tac_operand_t probe = { .kind = kind };
  if (kind == OPERAND_VAR) probe.name = name;
  else                     probe.temp_id = temp_id;
  if (zp_map_lookup(map, &probe) != 0)
    return;
  if (map->count >= ZP_MAP_MAX) return;

  unsigned size = codegen_type_size(type);
  zp_entry_t *e = &map->entries[map->count++];
  e->kind = kind;
  if (kind == OPERAND_VAR) e->name = name;
  else                     e->temp_id = temp_id;
  e->zp_addr = map->next_addr;
  e->size = (uint8_t)size;
  map->next_addr += (uint8_t)size;
}


static void zp_map_add_operand(zp_map_t *map, tac_operand_t *op) {
  if (op->kind == OPERAND_VAR)
    zp_map_add(map, OPERAND_VAR, op->name, 0, op->type);
  else if (op->kind == OPERAND_TEMP)
    zp_map_add(map, OPERAND_TEMP, NULL, op->temp_id, op->type);
}


static void zp_map_build(zp_map_t *map, cfg_t *cfg) {
  map->count = 0;
  map->next_addr = REG_START;

  for (unsigned i = 0; i < cfg->params.count; i++) {
    zp_map_add(map, OPERAND_VAR, cfg->params.items[i].name, 0,
               cfg->params.items[i].type);
  }

  for (unsigned i = 0; i < cfg->block_count; i++) {
    basic_block_t *block = cfg->blocks[i];
    for (unsigned j = 0; j < block->instr_count; j++) {
      tac_instr_t *inst = &block->instrs[j];
      zp_map_add_operand(map, &inst->dst);
      zp_map_add_operand(map, &inst->src1);
      zp_map_add_operand(map, &inst->src2);
    }
  }
}


// ----------------------------------------------------------------
// Label resolution
// ----------------------------------------------------------------

static void register_func_label(emitter_t *e, char *name, uint16_t addr) {
  if (e->func_label_count >= e->func_label_capacity) {
    unsigned cap = e->func_label_capacity ? e->func_label_capacity * 2 : 8;
    e->func_labels = realloc(e->func_labels, cap * sizeof(func_label_t));
    e->func_label_capacity = cap;
  }
  func_label_t *l = &e->func_labels[e->func_label_count++];
  l->name = name;
  l->addr = addr;
}


static int resolve_func_fixups(emitter_t *e) {
  for (unsigned i = 0; i < e->fixup_count; i++) {
    fixup_t *f = &e->fixups[i];
    uint16_t addr = 0;
    int found = 0;
    for (unsigned j = 0; j < e->func_label_count; j++) {
      if (strcmp(e->func_labels[j].name, f->func_name) == 0) {
        addr = e->func_labels[j].addr;
        found = 1;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "codegen: unresolved function '%s'\n", f->func_name);
      return 0;
    }
    e->rom[f->patch_pos]     = (uint8_t)(addr & 0xFF);
    e->rom[f->patch_pos + 1] = (uint8_t)(addr >> 8);
  }
  return 1;
}


static void add_fixup(emitter_t *e, char *func_name) {
  if (e->fixup_count >= e->fixup_capacity) {
    unsigned cap = e->fixup_capacity ? e->fixup_capacity * 2 : 8;
    e->fixups = realloc(e->fixups, cap * sizeof(fixup_t));
    e->fixup_capacity = cap;
  }
  fixup_t *f = &e->fixups[e->fixup_count++];
  f->patch_pos = e->code_pos;
  f->func_name = func_name;
  f->label_id = 0;
}


static int resolve_local_fixups(emitter_t *e) {
  for (unsigned i = 0; i < e->local_fixup_count; i++) {
    fixup_t *f = &e->local_fixups[i];
    uint16_t addr = e->local_labels[f->label_id];
    if (!addr) {
      fprintf(stderr, "codegen: unresolved local label L%u\n", f->label_id);
      return 0;
    }

    e->rom[f->patch_pos]     = (uint8_t)(addr & 0xFF);
    e->rom[f->patch_pos + 1] = (uint8_t)(addr >> 8);
  }
  return 1;
}


static void add_local_fixup(emitter_t *e, unsigned label_id) {
  if (e->local_fixup_count >= e->local_fixup_capacity) {
    unsigned cap = e->local_fixup_capacity ? e->local_fixup_capacity * 2 : 8;
    e->local_fixups = realloc(e->local_fixups, cap * sizeof(fixup_t));
    e->local_fixup_capacity = cap;
  }

  fixup_t *f = &e->local_fixups[e->local_fixup_count++];
  f->patch_pos = e->code_pos;
  f->label_id = label_id;
}


// ----------------------------------------------------------------
// Global symbol support
// ----------------------------------------------------------------

static global_entry_t *lookup_global(emitter_t *e, char *name) {
  for (unsigned i = 0; i < e->global_entry_count; i++) {
    if (strcmp(e->global_entries[i].name, name) == 0)
      return &e->global_entries[i];
  }
  return NULL;
}


static void allocate_globals(emitter_t *e, ir_gen_t *gen) {
  unsigned count = gen->module.global_count;
  if (count == 0) return;

  e->global_entries = malloc(count * sizeof(global_entry_t));
  e->global_entry_count = count;

  for (unsigned i = 0; i < count; i++) {
    ir_global_t *g = &gen->module.globals[i];
    unsigned size = codegen_type_size(g->type);
    global_entry_t *entry = &e->global_entries[i];
    entry->name = g->name;
    entry->ram_addr = e->ram_pos;
    entry->size = (uint8_t)size;
    entry->type = g->type;
    e->ram_pos += (uint16_t)size;
  }
}


// ----------------------------------------------------------------
// Op code emitters
// ----------------------------------------------------------------

#define EMIT(OP_CODE) e->rom[e->code_pos++] = OP_CODE

#define OP_EMITTER_SINGLE_ARG(NAME, OP_CODE)       \
  static void NAME(emitter_t *e, uint8_t byte) {   \
    EMIT(OP_CODE);                                 \
    EMIT(byte);                                    \
  }

#define OP_EMITTER_NO_ARG(NAME, OP_CODE)           \
  static void NAME(emitter_t *e) {                 \
    EMIT(OP_CODE);                                 \
  }

#define OP_EMITTER_ABS(NAME, OP_CODE)              \
  static void NAME(emitter_t *e, uint16_t addr) {  \
    EMIT(OP_CODE);                                 \
    EMIT((uint8_t)(addr & 0xFF));                  \
    EMIT((uint8_t)(addr >> 8));                    \
  }

OP_EMITTER_SINGLE_ARG(lda_imm, 0xA9)
OP_EMITTER_SINGLE_ARG(lda_zpg, 0xA5)
OP_EMITTER_SINGLE_ARG(lda_ind_y, 0xB1)
OP_EMITTER_SINGLE_ARG(ldx_imm, 0xA2)
OP_EMITTER_SINGLE_ARG(ldy_imm, 0xA0)
OP_EMITTER_SINGLE_ARG(sta_zpg, 0x85)

OP_EMITTER_SINGLE_ARG(cmp_imm, 0xC9)
OP_EMITTER_SINGLE_ARG(cmp_zpg, 0xC5)
OP_EMITTER_SINGLE_ARG(beq_rel, 0xF0)
OP_EMITTER_SINGLE_ARG(bne_rel, 0xD0)
OP_EMITTER_SINGLE_ARG(eor_imm, 0x49)

OP_EMITTER_SINGLE_ARG(inc_zpg, 0xE6)
OP_EMITTER_SINGLE_ARG(dec_zpg, 0xC6)

OP_EMITTER_SINGLE_ARG(adc_imm, 0x69)
OP_EMITTER_SINGLE_ARG(adc_zpg, 0x65)
OP_EMITTER_SINGLE_ARG(sbc_imm, 0xE9)
OP_EMITTER_SINGLE_ARG(sbc_zpg, 0xE5)

OP_EMITTER_NO_ARG(txs, 0x9A)
OP_EMITTER_NO_ARG(rts, 0x60)
OP_EMITTER_NO_ARG(clc, 0x18)
OP_EMITTER_NO_ARG(sec, 0x38)


OP_EMITTER_ABS(jmp_abs, 0x4C)
OP_EMITTER_ABS(sta_abs, 0x8D)
OP_EMITTER_ABS(lda_abs, 0xAD)
OP_EMITTER_ABS(cmp_abs, 0xCD)
OP_EMITTER_ABS(inc_abs, 0xEE)
OP_EMITTER_ABS(dec_abs, 0xCE)
OP_EMITTER_ABS(adc_abs, 0x6D)
OP_EMITTER_ABS(sbc_abs, 0xED)

#undef OP_EMITTER_SINGLE_ARG
#undef OP_EMITTER_NO_ARG
#undef OP_EMITTER_ABS


static void jsr(emitter_t *e, char *func_name) {
  EMIT(0x20);
  add_fixup(e, func_name);
  EMIT(0x00);
  EMIT(0x00);
}


// ----------------------------------------------------------------
// Global init & data section
// ----------------------------------------------------------------

static void add_data_fixup(emitter_t *e, unsigned global_idx, uint8_t byte) {
  if (e->data_fixup_count >= e->data_fixup_capacity) {
    unsigned cap = e->data_fixup_capacity ? e->data_fixup_capacity * 2 : 8;
    e->data_fixups = realloc(e->data_fixups, cap * sizeof(data_fixup_t));
    e->data_fixup_capacity = cap;
  }
  data_fixup_t *f = &e->data_fixups[e->data_fixup_count++];
  f->patch_pos = e->code_pos;
  f->global_idx = global_idx;
  f->byte = byte;
}


static void emit_global_init(emitter_t *e, ir_gen_t *gen) {
  for (unsigned i = 0; i < gen->module.global_count; i++) {
    ir_global_t *g = &gen->module.globals[i];
    global_entry_t *entry = &e->global_entries[i];
    unsigned width = entry->size;

    switch (g->init_kind) {
      case IR_INIT_INT:
        for (unsigned b = 0; b < width; b++) {
          lda_imm(e, (uint8_t)((g->int_val >> (8 * b)) & 0xFF));
          sta_abs(e, (uint16_t)(entry->ram_addr + b));
        }
        break;
      case IR_INIT_STR:
        for (unsigned b = 0; b < width; b++) {
          EMIT(0xA9);
          add_data_fixup(e, i, (uint8_t)b);
          EMIT(0x00);
          sta_abs(e, (uint16_t)(entry->ram_addr + b));
        }
        break;
      case IR_INIT_NONE:
        break;
    }
  }
}


static void emit_data_section(emitter_t *e, ir_gen_t *gen) {
  e->data_pos = e->code_pos;

  uint16_t *str_addrs = malloc(gen->module.global_count * sizeof(uint16_t));

  for (unsigned i = 0; i < gen->module.global_count; i++) {
    ir_global_t *g = &gen->module.globals[i];
    if (g->init_kind != IR_INIT_STR) {
      str_addrs[i] = 0;
      continue;
    }
    str_addrs[i] = (uint16_t)(ROM_START + e->code_pos);
    size_t len = strlen(g->str_val);
    for (size_t j = 0; j <= len; j++) {
      EMIT((uint8_t)g->str_val[j]);
    }
  }

  for (unsigned i = 0; i < e->data_fixup_count; i++) {
    data_fixup_t *f = &e->data_fixups[i];
    uint16_t addr = str_addrs[f->global_idx];
    e->rom[f->patch_pos] = (uint8_t)((addr >> (8 * f->byte)) & 0xFF);
  }

  free(str_addrs);
}


// ----------------------------------------------------------------
// High level emitters
// ----------------------------------------------------------------

static void emit_vectors(emitter_t *e) {
  unsigned pos = 0xFFFA - ROM_START;

  e->rom[pos++] = 0x00;               // NMI low  (unused)
  e->rom[pos++] = 0x00;               // NMI high (unused)
  e->rom[pos++] = ROM_START & 0xFF;   // Reset low
  e->rom[pos++] = ROM_START >> 8;     // Reset high
  e->rom[pos++] = 0x00;               // IRQ low  (unused)
  e->rom[pos++] = 0x00;               // IRQ high (unused)
}


static void emit_bootstrap(emitter_t *e) {
  EMIT(0x78); // SEI
  EMIT(0xD8); // CLD

  ldx_imm(e, 0XFF); // Init hardware stack
  txs(e);

  lda_imm(e, 0xFF); // init fp to point to top of hardware stack
  sta_zpg(e, FP);
  lda_imm(e, 0x01);
  sta_zpg(e, FP + 1);
}


static void emit_call_main(emitter_t *e) {
  jsr(e, "main");

  // halt loop
  uint16_t halt_addr = (uint16_t)(ROM_START + e->code_pos);
  jmp_abs(e, halt_addr);
}


static void emit_load_byte(emitter_t *e, zp_map_t *map,
                            tac_operand_t *op, unsigned byte) {
  switch (op->kind) {
    case OPERAND_CONST_INT:
      lda_imm(e, (uint8_t)((op->int_val >> (8 * byte)) & 0xFF));
      break;
    case OPERAND_VAR: {
      global_entry_t *g = lookup_global(e, op->name);
      if (g)
        lda_abs(e, (uint16_t)(g->ram_addr + byte));
      else
        lda_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    }
    case OPERAND_TEMP:
      lda_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    default: break;
  }
}


static void emit_store_byte(emitter_t *e, zp_map_t *map,
                             tac_operand_t *dst, unsigned byte) {
  if (dst->kind == OPERAND_VAR) {
    global_entry_t *g = lookup_global(e, dst->name);
    if (g) {
      sta_abs(e, (uint16_t)(g->ram_addr + byte));
      return;
    }
  }
  sta_zpg(e, (uint8_t)(zp_map_lookup(map, dst) + byte));
}


static void emit_cmp_byte(emitter_t *e, zp_map_t *map,
                          tac_operand_t *op, unsigned byte) {
  switch (op->kind) {
    case OPERAND_CONST_INT:
      cmp_imm(e, (uint8_t)((op->int_val >> (8 * byte)) & 0xFF));
      break;
    case OPERAND_VAR: {
      global_entry_t *g = lookup_global(e, op->name);
      if (g)
        cmp_abs(e, (uint16_t)(g->ram_addr + byte));
      else
        cmp_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    }
    case OPERAND_TEMP:
      cmp_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    default: break;
  }
}


static void emit_adc_byte(emitter_t *e, zp_map_t *map,
                          tac_operand_t *op, unsigned byte) {
  switch (op->kind) {
    case OPERAND_CONST_INT:
      adc_imm(e, (uint8_t)((op->int_val >> (8 * byte)) & 0xFF));
      break;
    case OPERAND_VAR: {
      global_entry_t *g = lookup_global(e, op->name);
      if (g)
        adc_abs(e, (uint16_t)(g->ram_addr + byte));
      else
        adc_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    }
    case OPERAND_TEMP:
      adc_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    default: break;
  }
}


static void emit_sbc_byte(emitter_t *e, zp_map_t *map,
                          tac_operand_t *op, unsigned byte) {
  switch (op->kind) {
    case OPERAND_CONST_INT:
      sbc_imm(e, (uint8_t)((op->int_val >> (8 * byte)) & 0xFF));
      break;
    case OPERAND_VAR: {
      global_entry_t *g = lookup_global(e, op->name);
      if (g)
        sbc_abs(e, (uint16_t)(g->ram_addr + byte));
      else
        sbc_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    }
    case OPERAND_TEMP:
      sbc_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    default: break;
  }
}


static void emit_cond_jump(emitter_t *e, zp_map_t *map,
                           tac_operand_t *src, unsigned label_id) {
  emit_load_byte(e, map, src, 0);
  beq_rel(e, 3);
  if (e->local_labels[label_id]) {
    jmp_abs(e, e->local_labels[label_id]);
  } else {
    EMIT(0x4C);
    add_local_fixup(e, label_id);
    EMIT(0x00);
    EMIT(0x00);
  }
}


static int emit_function_from_cfg(emitter_t *e, cfg_t *cfg) {
  register_func_label(e, cfg->name, (uint16_t)(ROM_START + e->code_pos));

  zp_map_t map;
  zp_map_build(&map, cfg);

  e->local_label_count = cfg->next_label;
  e->local_fixup_count = 0;
  e->local_labels = realloc(e->local_labels, cfg->next_label * sizeof(uint16_t));
  memset(e->local_labels, 0, cfg->next_label * sizeof(uint16_t));

  for (unsigned i = 0; i < cfg->block_count; ++i) {
    basic_block_t *block = cfg->blocks[i];

    for (unsigned j = 0; j < block->instr_count; ++j) {
      tac_instr_t *instruction = &block->instrs[j];

      switch (instruction->op) {
        // -- data movement --

        case TAC_COPY: {
          unsigned width = codegen_type_size(instruction->dst.type);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_LOAD: {
          unsigned width = codegen_type_size(instruction->dst.type);
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);

          if (instruction->src1.kind == OPERAND_CONST_INT) {
            uint16_t addr = (uint16_t)instruction->src1.int_val;
            for (unsigned b = 0; b < width; b++) {
              lda_abs(e, (uint16_t)(addr + b));
              sta_zpg(e, (uint8_t)(dst_addr + b));
            }
          } else {
            uint8_t ptr_zp = zp_map_lookup(&map, &instruction->src1);

            if (instruction->src1.kind == OPERAND_VAR) {
              global_entry_t *g = lookup_global(e, instruction->src1.name);
              if (g) {
                for (unsigned b = 0; b < 2; b++) {
                  lda_abs(e, (uint16_t)(g->ram_addr + b));
                  sta_zpg(e, (uint8_t)(ptr_zp + b));
                }
              }
            }

            for (unsigned b = 0; b < width; b++) {
              ldy_imm(e, (uint8_t)b);
              lda_ind_y(e, ptr_zp);
              sta_zpg(e, (uint8_t)(dst_addr + b));
            }
          }
          break;
        }

        case TAC_STORE: {
          if (instruction->dst.kind == OPERAND_CONST_INT) {
            unsigned width = codegen_type_size(instruction->src1.type);
            uint16_t base_addr = (uint16_t)instruction->dst.int_val;
            for (unsigned b = 0; b < width; b++) {
              emit_load_byte(e, &map, &instruction->src1, b);
              sta_abs(e, (uint16_t)(base_addr + b));
            }
          }
          break;
        }

        // -- control flow --

        case TAC_LABEL: e->local_labels[instruction->label] = (uint16_t)(ROM_START + e->code_pos); break;

        case TAC_JUMP: {
          if (e->local_labels[instruction->label]) {
            jmp_abs(e, e->local_labels[instruction->label]);
          } else {
            EMIT(0x4c);
            add_local_fixup(e, instruction->label);
            EMIT(0x00);
            EMIT(0x00);
          }
          break;
        }

        case TAC_COND_JUMP: emit_cond_jump(e, &map, &instruction->src1, instruction->label); break;

        case TAC_RETURN: {
          if (instruction->src1.kind != OPERAND_NONE) {
            unsigned width = codegen_type_size(cfg->return_type);
            for (unsigned b = 0; b < width; b++) {
              emit_load_byte(e, &map, &instruction->src1, b);
              sta_zpg(e, (uint8_t)(RET + b));
            }
          }
          rts(e);
          break;
        }

        // -- comparisons & boolean --

        case TAC_NOT: {
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
          emit_load_byte(e, &map, &instruction->src1, 0);
          eor_imm(e, 0x01);
          sta_zpg(e, dst_addr);
          break;
        }

        #define COMPARE_OP(TAC, LEFT, RIGHT, BRANCH_OP)                 \
          case TAC: {                                                   \
            uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);  \
            emit_load_byte(e, &map, &instruction->LEFT, 0);             \
            emit_cmp_byte(e, &map, &instruction->RIGHT, 0);             \
            EMIT(BRANCH_OP); EMIT(4);                                   \
            lda_imm(e, 0);                                              \
            EMIT(0xF0); EMIT(2);                                        \
            lda_imm(e, 1);                                              \
            sta_zpg(e, dst_addr);                                       \
            break;                                                      \
          }

        COMPARE_OP(TAC_LT,  src1, src2, 0x90)  // BCC — true if <
        COMPARE_OP(TAC_GTE, src1, src2, 0xB0)  // BCS — true if >=
        COMPARE_OP(TAC_EQ,  src1, src2, 0xF0)  // BEQ — true if ==
        COMPARE_OP(TAC_NEQ, src1, src2, 0xD0)  // BNE — true if !=
        COMPARE_OP(TAC_GT,  src2, src1, 0x90)  // a>b = b<a, swap+BCC
        COMPARE_OP(TAC_LTE, src2, src1, 0xB0)  // a<=b = b>=a, swap+BCS
        #undef COMPARE_OP

        // -- increment / decrement --

        case TAC_INC: {
          unsigned width = codegen_type_size(instruction->dst.type);
          global_entry_t *g = (instruction->dst.kind == OPERAND_VAR)
            ? lookup_global(e, instruction->dst.name) : NULL;
          if (g) {
            inc_abs(e, g->ram_addr);
            if (width > 1) {
              bne_rel(e, 3);
              inc_abs(e, (uint16_t)(g->ram_addr + 1));
            }
          } else {
            uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
            inc_zpg(e, dst_addr);
            if (width > 1) {
              bne_rel(e, 2);
              inc_zpg(e, (uint8_t)(dst_addr + 1));
            }
          }
          break;
        }

        case TAC_DEC: {
          unsigned width = codegen_type_size(instruction->dst.type);
          global_entry_t *g = (instruction->dst.kind == OPERAND_VAR)
            ? lookup_global(e, instruction->dst.name) : NULL;
          if (g) {
            if (width > 1) {
              lda_abs(e, g->ram_addr);
              bne_rel(e, 3);
              dec_abs(e, (uint16_t)(g->ram_addr + 1));
            }
            dec_abs(e, g->ram_addr);
          } else {
            uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
            if (width > 1) {
              lda_zpg(e, dst_addr);
              bne_rel(e, 2);
              dec_zpg(e, (uint8_t)(dst_addr + 1));
            }
            dec_zpg(e, dst_addr);
          }
          break;
        }

        // -- arithmetic --

        case TAC_ADD: {
          unsigned width = codegen_type_size(instruction->dst.type);
          clc(e);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_adc_byte(e, &map, &instruction->src2, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_SUB: {
          unsigned width = codegen_type_size(instruction->dst.type);
          sec(e);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_sbc_byte(e, &map, &instruction->src2, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        default:
          fprintf(stderr, "codegen: unhandled TAC op %d\n", instruction->op);
          return 0;
      }
    }
  }

  return resolve_local_fixups(e);
}


// ----------------------------------------------------------------
// Main code gen
// ----------------------------------------------------------------

static void emitter_free(emitter_t *e) {
  free(e->func_labels);
  free(e->fixups);
  free(e->local_labels);
  free(e->local_fixups);
  free(e->global_entries);
  free(e->data_fixups);
}


uint8_t *generate_rom(ir_gen_t *gen, size_t *final_rom_size) {
  emitter_t e = { 0 };

  e.rom = malloc(ROM_SIZE);
  if (!e.rom) return NULL;

  memset(e.rom, 0xEA, ROM_SIZE);
  *final_rom_size = ROM_SIZE;

  e.ram_pos = RAM_START;
  e.zp_next = REG_START;

  allocate_globals(&e, gen);
  emit_bootstrap(&e);
  emit_global_init(&e, gen);
  emit_call_main(&e);

  for (unsigned i = 0; i < gen->module.cfg_count; ++i) {
    if (!emit_function_from_cfg(&e, &gen->module.cfgs[i])) {
      free(e.rom);
      emitter_free(&e);
      *final_rom_size = 0;
      return NULL;
    }
  }

  if (!resolve_func_fixups(&e)) {
    free(e.rom);
    emitter_free(&e);
    *final_rom_size = 0;
    return NULL;
  }

  emit_data_section(&e, gen);

  // code/data boundary at $FFF8 for the disassembler
  unsigned boundary_pos = 0xFFF8 - ROM_START;
  uint16_t code_end = (uint16_t)(ROM_START + e.data_pos);
  e.rom[boundary_pos]     = (uint8_t)(code_end & 0xFF);
  e.rom[boundary_pos + 1] = (uint8_t)(code_end >> 8);

  emit_vectors(&e);
  emitter_free(&e);

  return e.rom;
}