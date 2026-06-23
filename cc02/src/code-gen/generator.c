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
OP_EMITTER_SINGLE_ARG(ldx_imm, 0xA2)
OP_EMITTER_SINGLE_ARG(sta_zpg, 0x85)

OP_EMITTER_SINGLE_ARG(cmp_imm, 0xC9)
OP_EMITTER_SINGLE_ARG(cmp_zpg, 0xC5)
OP_EMITTER_SINGLE_ARG(beq_rel, 0xF0)
OP_EMITTER_SINGLE_ARG(eor_imm, 0x49)

OP_EMITTER_SINGLE_ARG(inc_zpg, 0xE6)
OP_EMITTER_SINGLE_ARG(dec_zpg, 0xC6)

OP_EMITTER_NO_ARG(txs, 0x9A)
OP_EMITTER_NO_ARG(rts, 0x60)


OP_EMITTER_ABS(jmp_abs, 0x4C)
OP_EMITTER_ABS(sta_abs, 0x8D)

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


static void emit_runtime(emitter_t *e) {
  EMIT(0x78); // SEI
  EMIT(0xD8); // CLD

  ldx_imm(e, 0XFF); // Init hardware stack
  txs(e);

  lda_imm(e, 0xFF); // init fp to point to top of hardware stack
  sta_zpg(e, FP);
  lda_imm(e, 0x01);
  sta_zpg(e, FP + 1);

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
    case OPERAND_VAR:
    case OPERAND_TEMP:
      lda_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
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


static void emit_function_from_cfg(emitter_t *e, cfg_t *cfg) {
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
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            sta_zpg(e, (uint8_t)(dst_addr + b));
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
            if (instruction->RIGHT.kind == OPERAND_CONST_INT)           \
              cmp_imm(e, (uint8_t)(instruction->RIGHT.int_val & 0xFF)); \
            else                                                        \
              cmp_zpg(e, zp_map_lookup(&map, &instruction->RIGHT));     \
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
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
          inc_zpg(e, dst_addr);
          break;
        }

        case TAC_DEC: {
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
          dec_zpg(e, dst_addr);
          break;
        }

        default: break;
      }
    }
  }

  resolve_local_fixups(e);
  rts(e);
}


// ----------------------------------------------------------------
// Main code gen
// ----------------------------------------------------------------

static void emitter_free(emitter_t *e) {
  free(e->func_labels);
  free(e->fixups);
  free(e->local_labels);
  free(e->local_fixups);
}


uint8_t *generate_rom(ir_gen_t *gen, size_t *final_rom_size) {
  emitter_t e = { 0 };

  e.rom = malloc(ROM_SIZE);
  if (!e.rom) return NULL;

  memset(e.rom, 0xEA, ROM_SIZE);
  *final_rom_size = ROM_SIZE;

  e.ram_start = RAM_START;
  e.zp_next = REG_START;

  emit_runtime(&e);

  // walk cfg blocks and emit functions
  for (unsigned i = 0; i < gen->module.cfg_count; ++i) {
    emit_function_from_cfg(&e, &gen->module.cfgs[i]);
  }

  if (!resolve_func_fixups(&e)) {
    free(e.rom);
    emitter_free(&e);
    *final_rom_size = 0;
    return NULL;
  }

  emit_vectors(&e);
  emitter_free(&e);

  return e.rom;
}