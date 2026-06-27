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

// ABI zone: fixed 2-byte slots per parameter ($EF/$F0, $F1/$F2, ..., $FD/$FE).
// Using fixed 2-byte slots simplifies caller/callee agreement at the cost of 1 byte
// per u8 parameter. Callee-saves via PHA/PLA preserves the caller's ZP slots across
// the call, which also enables bounded recursion (limited by the 256-byte hw stack).
// NOTE: signed narrower→wider widening (e.g. i8 arg into i16 param) is zero-extended
// at the call site, not sign-extended. The IR does not insert explicit cast nodes for
// implicit widening in call arguments. For now this means negative i8 args passed to
// i16 params produce wrong values; a future IR pass should insert TAC_CAST nodes here.
#define ABI_SLOT_SIZE  2
#define ABI_MAX_PARAMS 8

// Fixed ZP slots for arithmetic helper subroutines ($E8–$EB, below PARAM_START).
#define HELPER_ARG1 0xE8  // dividend / multiplicand (modified by helpers)
#define HELPER_ARG2 0xE9  // divisor  / multiplier   (modified by helpers)
#define HELPER_RES  0xEA  // quotient / product
#define HELPER_REM  0xEB  // remainder (division only)
#define HELPER_SIGN 0xEC  // sign flags for __sdiv8/__sdiv16 (bit7=negate quotient, bit6=negate remainder)

// 16-bit helper ZP slots ($E0–$E7, below the 8-bit helper zone at $E8)
#define HELPER16_ARG1 0xE0  // 2 bytes: dividend/multiplicand (lo=$E0, hi=$E1)
#define HELPER16_ARG2 0xE2  // 2 bytes: divisor/multiplier   (lo=$E2, hi=$E3)
#define HELPER16_RES  0xE4  // 2 bytes: quotient/product     (lo=$E4, hi=$E5)
#define HELPER16_REM  0xE6  // 2 bytes: remainder            (lo=$E6, hi=$E7)


// ----------------------------------------------------------------
// Zero-page operand map
// ----------------------------------------------------------------

// Return byte width for a type (1 for u8/i8, 2 for u16/i16/pointers).
static unsigned codegen_type_size(type_t type) {
  if (type.is_ptr) return 2;
  switch (type.kind) {
    case TYPE_U8:  case TYPE_I8:  return 1;
    case TYPE_U16: case TYPE_I16: return 2;
    default: return 1;
  }
}

static unsigned full_type_size(emitter_t *e, type_t type) {
  if (type.is_ptr) return 2;
  if (type.kind == TYPE_STRUCT && e->gen) {
    for (unsigned i = 0; i < e->gen->module.struct_count; i++) {
      if (strcmp(e->gen->module.structs[i].name, type.struct_name) == 0)
        return e->gen->module.structs[i].total_size;
    }
  }
  return codegen_type_size(type);
}

static ir_field_def_t *lookup_struct_field(emitter_t *e,
                                            const char *struct_name,
                                            const char *field_name) {
  for (unsigned i = 0; i < e->gen->module.struct_count; i++) {
    ir_struct_def_t *s = &e->gen->module.structs[i];
    if (strcmp(s->name, struct_name) != 0) continue;
    for (unsigned j = 0; j < s->field_count; j++) {
      if (strcmp(s->fields[j].name, field_name) == 0)
        return &s->fields[j];
    }
  }
  return NULL;
}


// True if the type requires signed comparison semantics.
static int is_signed_type(type_t type) {
  return type.kind == TYPE_I8 || type.kind == TYPE_I16;
}


// Sentinel for "operand not in ZP map". Must be a ZP address that is never
// assigned by the map — $FF lives in the ABI param zone ($EF–$FF).
#define ZP_NOT_FOUND 0xFF

// Find the ZP slot assigned to a var/temp operand. Returns ZP_NOT_FOUND if not found.
static uint8_t zp_map_lookup(zp_map_t *map, tac_operand_t *op) {
  for (unsigned i = 0; i < map->count; i++) {
    zp_entry_t *e = &map->entries[i];
    if (e->kind != op->kind) continue;
    if (e->kind == OPERAND_VAR && strcmp(e->name, op->name) == 0) return e->zp_addr;
    if (e->kind == OPERAND_TEMP && e->temp_id == op->temp_id)    return e->zp_addr;
  }
  return ZP_NOT_FOUND;
}


// Assign the next available ZP slot to an operand (deduped, type-stride-aware).
static void zp_map_add(emitter_t *e, zp_map_t *map, tac_operand_kind_t kind,
                        char *name, unsigned temp_id, type_t type) {
  tac_operand_t probe = { .kind = kind };
  if (kind == OPERAND_VAR) probe.name = name;
  else                     probe.temp_id = temp_id;
  if (zp_map_lookup(map, &probe) != ZP_NOT_FOUND)
    return;
  if (map->count >= ZP_MAP_MAX) {
    fprintf(stderr, "codegen: ZP map overflow (>%d operands)\n", ZP_MAP_MAX);
    return;
  }

  unsigned size = full_type_size(e, type);
  if ((unsigned)map->next_addr + size > HELPER16_ARG1) {
    fprintf(stderr, "codegen: ZP space exhausted (next=$%02X, need %u bytes, limit=$%02X)\n",
            map->next_addr, size, HELPER16_ARG1);
    return;
  }

  zp_entry_t *entry = &map->entries[map->count++];
  entry->kind = kind;
  if (kind == OPERAND_VAR) entry->name = name;
  else                     entry->temp_id = temp_id;
  entry->zp_addr = map->next_addr;
  entry->size = (uint8_t)size;
  map->next_addr += (uint8_t)size;
}


// Register a TAC operand in the ZP map (dispatches var vs temp).
static void zp_map_add_operand(emitter_t *e, zp_map_t *map, tac_operand_t *op) {
  if (op->kind == OPERAND_VAR)
    zp_map_add(e, map, OPERAND_VAR, op->name, 0, op->type);
  else if (op->kind == OPERAND_TEMP)
    zp_map_add(e, map, OPERAND_TEMP, NULL, op->temp_id, op->type);
}


// Build the per-function ZP map: params first, then all referenced operands.
static void zp_map_build(emitter_t *e, zp_map_t *map, cfg_t *cfg) {
  map->count = 0;
  map->next_addr = REG_START;

  for (unsigned i = 0; i < cfg->params.count; i++) {
    zp_map_add(e, map, OPERAND_VAR, cfg->params.items[i].name, 0,
               cfg->params.items[i].type);
  }

  for (unsigned i = 0; i < cfg->block_count; i++) {
    basic_block_t *block = cfg->blocks[i];
    for (unsigned j = 0; j < block->instr_count; j++) {
      tac_instr_t *inst = &block->instrs[j];
      zp_map_add_operand(e, map, &inst->dst);
      zp_map_add_operand(e, map, &inst->src1);
      zp_map_add_operand(e, map, &inst->src2);
    }
  }
}


// ----------------------------------------------------------------
// Label resolution
// ----------------------------------------------------------------

// Record a function's ROM address for JSR fixup resolution.
static void register_func_label(emitter_t *e, char *name, uint16_t addr) {
  if (e->func_label_count >= e->func_label_capacity) {
    unsigned cap = e->func_label_capacity ? e->func_label_capacity * 2 : 8;
    func_label_t *grown = arena_alloc(&e->arena, cap * sizeof(func_label_t));
    if (e->func_labels)
      memcpy(grown, e->func_labels, e->func_label_count * sizeof(func_label_t));
    e->func_labels = grown;
    e->func_label_capacity = cap;
  }
  func_label_t *l = &e->func_labels[e->func_label_count++];
  l->name = name;
  l->addr = addr;
}


// Backpatch all JSR placeholders with resolved function addresses.
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


// Queue a forward-reference fixup for a JSR to an unresolved function.
static void add_fixup(emitter_t *e, char *func_name) {
  if (e->fixup_count >= e->fixup_capacity) {
    unsigned cap = e->fixup_capacity ? e->fixup_capacity * 2 : 8;
    fixup_t *grown = arena_alloc(&e->arena, cap * sizeof(fixup_t));
    if (e->fixups)
      memcpy(grown, e->fixups, e->fixup_count * sizeof(fixup_t));
    e->fixups = grown;
    e->fixup_capacity = cap;
  }
  fixup_t *f = &e->fixups[e->fixup_count++];
  f->patch_pos = e->code_pos;
  f->func_name = func_name;
  f->label_id = 0;
}


// Backpatch all local label placeholders (JMP/COND_JUMP) within a function.
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


// Queue a forward-reference fixup for a local control-flow label.
static void add_local_fixup(emitter_t *e, unsigned label_id) {
  if (e->local_fixup_count >= e->local_fixup_capacity) {
    unsigned cap = e->local_fixup_capacity ? e->local_fixup_capacity * 2 : 8;
    fixup_t *grown = arena_alloc(&e->arena, cap * sizeof(fixup_t));
    if (e->local_fixups)
      memcpy(grown, e->local_fixups, e->local_fixup_count * sizeof(fixup_t));
    e->local_fixups = grown;
    e->local_fixup_capacity = cap;
  }

  fixup_t *f = &e->local_fixups[e->local_fixup_count++];
  f->patch_pos = e->code_pos;
  f->label_id = label_id;
}


// ----------------------------------------------------------------
// Global symbol support
// ----------------------------------------------------------------

// Look up a global variable by name; returns NULL for locals/temps.
static global_entry_t *lookup_global(emitter_t *e, char *name) {
  for (unsigned i = 0; i < e->global_entry_count; i++) {
    if (strcmp(e->global_entries[i].name, name) == 0)
      return &e->global_entries[i];
  }
  return NULL;
}


// Assign RAM addresses ($0200+) to each global variable with type-aware stride.
static void allocate_globals(emitter_t *e, ir_gen_t *gen) {
  unsigned count = gen->module.global_count;
  if (count == 0) return;

  e->global_entries = arena_alloc(&e->arena, count * sizeof(global_entry_t));
  e->global_entry_count = count;

  for (unsigned i = 0; i < count; i++) {
    ir_global_t *g = &gen->module.globals[i];
    unsigned size = full_type_size(e, g->type);
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

OP_EMITTER_SINGLE_ARG(lda_imm,   0xA9)
OP_EMITTER_SINGLE_ARG(lda_zpg,   0xA5)
OP_EMITTER_SINGLE_ARG(lda_ind_y, 0xB1)
OP_EMITTER_SINGLE_ARG(sta_ind_y, 0x91)
OP_EMITTER_SINGLE_ARG(ldx_imm,   0xA2)
OP_EMITTER_SINGLE_ARG(ldy_imm,   0xA0)
OP_EMITTER_SINGLE_ARG(sta_zpg,   0x85)

OP_EMITTER_SINGLE_ARG(ora_imm, 0x09)
OP_EMITTER_SINGLE_ARG(ora_zpg, 0x05)
OP_EMITTER_SINGLE_ARG(and_imm, 0x29)
OP_EMITTER_SINGLE_ARG(and_zpg, 0x25)
OP_EMITTER_SINGLE_ARG(eor_imm, 0x49)
OP_EMITTER_SINGLE_ARG(eor_zpg, 0x45)

OP_EMITTER_SINGLE_ARG(asl_zpg, 0x06)
OP_EMITTER_SINGLE_ARG(rol_zpg, 0x26)
OP_EMITTER_SINGLE_ARG(lsr_zpg, 0x46)
OP_EMITTER_SINGLE_ARG(ror_zpg, 0x66)

OP_EMITTER_SINGLE_ARG(cmp_imm, 0xC9)
OP_EMITTER_SINGLE_ARG(cmp_zpg, 0xC5)
OP_EMITTER_SINGLE_ARG(beq_rel, 0xF0)
OP_EMITTER_SINGLE_ARG(bne_rel, 0xD0)
OP_EMITTER_SINGLE_ARG(bcs_rel, 0xB0)
OP_EMITTER_SINGLE_ARG(bcc_rel, 0x90)
OP_EMITTER_SINGLE_ARG(bpl_rel, 0x10)

OP_EMITTER_SINGLE_ARG(inc_zpg, 0xE6)
OP_EMITTER_SINGLE_ARG(dec_zpg, 0xC6)

OP_EMITTER_SINGLE_ARG(adc_imm, 0x69)
OP_EMITTER_SINGLE_ARG(adc_zpg, 0x65)
OP_EMITTER_SINGLE_ARG(sbc_imm, 0xE9)
OP_EMITTER_SINGLE_ARG(sbc_zpg, 0xE5)

OP_EMITTER_SINGLE_ARG(bvc_rel, 0x50)

OP_EMITTER_NO_ARG(txs, 0x9A)
OP_EMITTER_NO_ARG(rts, 0x60)
OP_EMITTER_NO_ARG(clc, 0x18)
OP_EMITTER_NO_ARG(sec, 0x38)
OP_EMITTER_NO_ARG(tax, 0xAA)
OP_EMITTER_NO_ARG(dex, 0xCA)
OP_EMITTER_NO_ARG(pha, 0x48)
OP_EMITTER_NO_ARG(pla, 0x68)


OP_EMITTER_ABS(jmp_abs, 0x4C)
OP_EMITTER_ABS(sta_abs, 0x8D)
OP_EMITTER_ABS(lda_abs, 0xAD)
OP_EMITTER_ABS(ora_abs, 0x0D)
OP_EMITTER_ABS(and_abs, 0x2D)
OP_EMITTER_ABS(eor_abs, 0x4D)
OP_EMITTER_ABS(cmp_abs, 0xCD)
OP_EMITTER_ABS(inc_abs, 0xEE)
OP_EMITTER_ABS(dec_abs, 0xCE)
OP_EMITTER_ABS(adc_abs, 0x6D)
OP_EMITTER_ABS(sbc_abs, 0xED)

#undef OP_EMITTER_SINGLE_ARG
#undef OP_EMITTER_NO_ARG
#undef OP_EMITTER_ABS


// Emit JSR with a placeholder address and queue a fixup for later resolution.
static void jsr(emitter_t *e, char *func_name) {
  EMIT(0x20);
  add_fixup(e, func_name);
  EMIT(0x00);
  EMIT(0x00);
}


// ----------------------------------------------------------------
// Global init & data section
// ----------------------------------------------------------------

// Queue a fixup for a string ROM address (resolved after data section is emitted).
static void add_data_fixup(emitter_t *e, unsigned global_idx, uint8_t byte) {
  if (e->data_fixup_count >= e->data_fixup_capacity) {
    unsigned cap = e->data_fixup_capacity ? e->data_fixup_capacity * 2 : 8;
    data_fixup_t *grown = arena_alloc(&e->arena, cap * sizeof(data_fixup_t));
    if (e->data_fixups)
      memcpy(grown, e->data_fixups, e->data_fixup_count * sizeof(data_fixup_t));
    e->data_fixups = grown;
    e->data_fixup_capacity = cap;
  }
  data_fixup_t *f = &e->data_fixups[e->data_fixup_count++];
  f->patch_pos = e->code_pos;
  f->global_idx = global_idx;
  f->byte = byte;
}


// Emit bootstrap code to initialize each global variable at its RAM address.
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


// Write string literals into ROM after code and resolve data fixups.
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

// Write NMI, Reset, and IRQ vectors at $FFFA-$FFFF.
static void emit_vectors(emitter_t *e) {
  unsigned pos = 0xFFFA - ROM_START;

  e->rom[pos++] = 0x00;               // NMI low  (unused)
  e->rom[pos++] = 0x00;               // NMI high (unused)
  e->rom[pos++] = ROM_START & 0xFF;   // Reset low
  e->rom[pos++] = ROM_START >> 8;     // Reset high
  e->rom[pos++] = 0x00;               // IRQ low  (unused)
  e->rom[pos++] = 0x00;               // IRQ high (unused)
}


// Emit the reset stub: SEI, CLD, stack init, frame pointer init.
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


// Emit JSR main followed by an infinite halt loop (JMP to self).
static void emit_call_main(emitter_t *e) {
  jsr(e, "main");

  // halt loop
  uint16_t halt_addr = (uint16_t)(ROM_START + e->code_pos);
  jmp_abs(e, halt_addr);
}


// Load byte N of an operand into A. Global-aware: uses abs for globals, zpg for locals.
static void emit_load_byte(emitter_t *e, zp_map_t *map,
                            tac_operand_t *op, unsigned byte) {
  switch (op->kind) {
    case OPERAND_CONST_INT:
      lda_imm(e, (uint8_t)((op->int_val >> (8 * byte)) & 0xFF));
      break;
    case OPERAND_VAR: {
      if (byte >= full_type_size(e, op->type)) { lda_imm(e, 0); break; }
      global_entry_t *g = lookup_global(e, op->name);
      if (g)
        lda_abs(e, (uint16_t)(g->ram_addr + byte));
      else
        lda_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    }
    case OPERAND_TEMP:
      if (byte >= full_type_size(e, op->type)) { lda_imm(e, 0); break; }
      lda_zpg(e, (uint8_t)(zp_map_lookup(map, op) + byte));
      break;
    default: break;
  }
}


// Store A into byte N of a destination. Global-aware: uses abs for globals, zpg for locals.
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


#define GLOBAL_AWARE_ALU_HELPER(NAME, IMM_FN, ZPG_FN, ABS_FN)            \
  static void NAME(emitter_t *e, zp_map_t *map,                          \
                   tac_operand_t *op, unsigned byte) {                   \
    switch (op->kind) {                                                  \
      case OPERAND_CONST_INT:                                            \
        IMM_FN(e, (uint8_t)((op->int_val >> (8 * byte)) & 0xFF));        \
        break;                                                           \
      case OPERAND_VAR: {                                                \
        if (byte >= full_type_size(e, op->type)) { IMM_FN(e, 0); break; } \
        global_entry_t *g = lookup_global(e, op->name);                  \
        if (g)                                                           \
          ABS_FN(e, (uint16_t)(g->ram_addr + byte));                     \
        else                                                             \
          ZPG_FN(e, (uint8_t)(zp_map_lookup(map, op) + byte));           \
        break;                                                           \
      }                                                                  \
      case OPERAND_TEMP:                                                 \
        if (byte >= full_type_size(e, op->type)) { IMM_FN(e, 0); break; } \
        ZPG_FN(e, (uint8_t)(zp_map_lookup(map, op) + byte));             \
        break;                                                           \
      default: break;                                                    \
    }                                                                    \
  }

GLOBAL_AWARE_ALU_HELPER(emit_ora_byte, ora_imm, ora_zpg, ora_abs)
GLOBAL_AWARE_ALU_HELPER(emit_and_byte, and_imm, and_zpg, and_abs)
GLOBAL_AWARE_ALU_HELPER(emit_eor_byte, eor_imm, eor_zpg, eor_abs)
GLOBAL_AWARE_ALU_HELPER(emit_cmp_byte, cmp_imm, cmp_zpg, cmp_abs)
GLOBAL_AWARE_ALU_HELPER(emit_adc_byte, adc_imm, adc_zpg, adc_abs)
GLOBAL_AWARE_ALU_HELPER(emit_sbc_byte, sbc_imm, sbc_zpg, sbc_abs)

// Emit conditional jump: LDA src; [ORA src+1]; BEQ skip; JMP target. Jumps when nonzero.
static void emit_cond_jump(emitter_t *e, zp_map_t *map,
                           tac_operand_t *src, unsigned label_id) {
  unsigned width = codegen_type_size(src->type);
  emit_load_byte(e, map, src, 0);
  if (width > 1)
    emit_ora_byte(e, map, src, 1);
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


// Push every byte of every ZP slot this function uses onto the hardware stack.
// Called at function entry (before the ABI-zone copy) so that whatever the caller
// had at those ZP addresses is preserved across the call.
static void emit_zp_save(emitter_t *e, zp_map_t *map) {
  for (unsigned i = 0; i < map->count; i++) {
    for (unsigned b = 0; b < map->entries[i].size; b++) {
      lda_zpg(e, (uint8_t)(map->entries[i].zp_addr + b));
      pha(e);
    }
  }
}

// Pop every byte back in reverse order (LIFO) and store into the ZP slots.
// Called before every RTS so the caller's ZP values are restored on return.
static void emit_zp_restore(emitter_t *e, zp_map_t *map) {
  unsigned i = map->count;
  while (i--) {
    unsigned b = map->entries[i].size;
    while (b--) {
      pla(e);
      sta_zpg(e, (uint8_t)(map->entries[i].zp_addr + b));
    }
  }
}


// Copy each argument from the fixed ABI zone into the function's own ZP slots.
// Skipped when param_count == 0 (covers main and any no-arg function).
static int emit_function_prologue(emitter_t *e, zp_map_t *map, cfg_t *cfg) {
  if (cfg->params.count > ABI_MAX_PARAMS) {
    fprintf(stderr, "codegen: function '%s' has %u parameters, max is %d\n",
            cfg->name, cfg->params.count, ABI_MAX_PARAMS);
    return 0;
  }
  for (unsigned i = 0; i < cfg->params.count; i++) {
    param_t *param = &cfg->params.items[i];
    unsigned size = codegen_type_size(param->type);
    tac_operand_t op = { .kind = OPERAND_VAR, .name = param->name, .type = param->type };
    uint8_t zp = zp_map_lookup(map, &op);
    for (unsigned b = 0; b < size; b++) {
      lda_zpg(e, (uint8_t)(PARAM_START + i * ABI_SLOT_SIZE + b));
      sta_zpg(e, (uint8_t)(zp + b));
    }
  }
  return 1;
}


// Lower a function's TAC instruction stream to 65C02 machine code.
static int emit_function_from_cfg(emitter_t *e, cfg_t *cfg) {
  register_func_label(e, cfg->name, (uint16_t)(ROM_START + e->code_pos));

  zp_map_t map;
  zp_map_build(e, &map, cfg);

  // main is only called by the bootstrap, which has no ZP state to preserve.
  // Every other callee saves the caller's ZP slots on entry and restores on return.
  int is_main = (strcmp(cfg->name, "main") == 0);
  if (!is_main)
    emit_zp_save(e, &map);

  if (cfg->params.count > 0 && !emit_function_prologue(e, &map, cfg))
    return 0;

  e->local_label_count = cfg->next_label;
  e->local_fixup_count = 0;
  e->local_labels = arena_alloc(&e->arena, cfg->next_label * sizeof(uint16_t));

  for (unsigned i = 0; i < cfg->block_count; ++i) {
    basic_block_t *block = cfg->blocks[i];

    for (unsigned j = 0; j < block->instr_count; ++j) {
      tac_instr_t *instruction = &block->instrs[j];

      switch (instruction->op) {
        // -- data movement --

        case TAC_COPY: {
          unsigned src_size = full_type_size(e, instruction->src1.type);
          unsigned dst_size = full_type_size(e, instruction->dst.type);
          unsigned copy_size = src_size < dst_size ? src_size : dst_size;

          for (unsigned b = 0; b < copy_size; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }

          // Implicit widening: extend hi bytes (A holds hi byte of src after loop)
          if (dst_size > src_size) {
            if (is_signed_type(instruction->src1.type)) {
              cmp_imm(e, 0x80);
              lda_imm(e, 0xFF);
              bcs_rel(e, 2);
              lda_imm(e, 0);
            } else {
              lda_imm(e, 0);
            }
            for (unsigned b = src_size; b < dst_size; b++)
              emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_CAST: {
          unsigned src_size = codegen_type_size(instruction->src1.type);
          unsigned dst_size = codegen_type_size(instruction->cast_type);
          uint8_t dst_zp = zp_map_lookup(&map, &instruction->dst);
          unsigned copy_size = src_size < dst_size ? src_size : dst_size;

          for (unsigned b = 0; b < copy_size; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            sta_zpg(e, (uint8_t)(dst_zp + b));
          }

          if (dst_size > src_size) {
            // After the loop, A holds the high byte of src. Use it to extend.
            if (is_signed_type(instruction->src1.type)) {
              // Sign-extend: CMP #$80 sets C if src is negative (bit 7 = 1).
              // A = $FF if negative, $00 if positive; BCS skips the LDA #0.
              cmp_imm(e, 0x80);
              lda_imm(e, 0xFF);
              bcs_rel(e, 2);  // skip LDA #0 (2 bytes) if carry set (negative)
              lda_imm(e, 0);
            } else {
              lda_imm(e, 0);
            }
            for (unsigned b = src_size; b < dst_size; b++)
              sta_zpg(e, (uint8_t)(dst_zp + b));
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
          unsigned width = codegen_type_size(instruction->src1.type);
          if (instruction->dst.kind == OPERAND_CONST_INT) {
            uint16_t base_addr = (uint16_t)instruction->dst.int_val;
            for (unsigned b = 0; b < width; b++) {
              emit_load_byte(e, &map, &instruction->src1, b);
              sta_abs(e, (uint16_t)(base_addr + b));
            }
          } else {
            uint8_t ptr_zp = zp_map_lookup(&map, &instruction->dst);
            if (instruction->dst.kind == OPERAND_VAR) {
              global_entry_t *g = lookup_global(e, instruction->dst.name);
              if (g) {
                for (unsigned b = 0; b < 2; b++) {
                  lda_abs(e, (uint16_t)(g->ram_addr + b));
                  sta_zpg(e, (uint8_t)(ptr_zp + b));
                }
              }
            }
            for (unsigned b = 0; b < width; b++) {
              ldy_imm(e, (uint8_t)b);
              emit_load_byte(e, &map, &instruction->src1, b);
              sta_ind_y(e, ptr_zp);
            }
          }
          break;
        }

        case TAC_ADDR_OF: {
          uint8_t dst_zp = zp_map_lookup(&map, &instruction->dst);
          uint16_t addr;
          global_entry_t *g = lookup_global(e, instruction->src1.name);
          if (g) {
            addr = g->ram_addr;
          } else {
            addr = (uint16_t)zp_map_lookup(&map, &instruction->src1);
          }
          lda_imm(e, (uint8_t)(addr & 0xFF));
          sta_zpg(e, dst_zp);
          lda_imm(e, (uint8_t)(addr >> 8));
          sta_zpg(e, (uint8_t)(dst_zp + 1));
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
          // Restore the caller's ZP slots before returning (balances emit_zp_save).
          // Return value is already in RET ($02/$03) above the restored region.
          if (!is_main)
            emit_zp_restore(e, &map);
          rts(e);
          break;
        }

        case TAC_CALL: {
          if (instruction->call_arg_count > ABI_MAX_PARAMS) {
            fprintf(stderr, "codegen: call to '%s' passes %u arguments, max is %d\n",
                    instruction->call_name, instruction->call_arg_count, ABI_MAX_PARAMS);
            return 0;
          }
          // Copy each argument into its fixed 2-byte ABI zone slot. emit_load_byte
          // zero-extends naturally for byte indices past the operand's type width.
          for (unsigned ai = 0; ai < instruction->call_arg_count; ai++) {
            tac_operand_t *arg = &instruction->call_args[ai];
            for (unsigned ab = 0; ab < ABI_SLOT_SIZE; ab++) {
              emit_load_byte(e, &map, arg, ab);
              sta_zpg(e, (uint8_t)(PARAM_START + ai * ABI_SLOT_SIZE + ab));
            }
          }
          jsr(e, instruction->call_name);
          // Copy return value from ZP_RET into the destination temp (skip for void).
          if (instruction->dst.type.kind != TYPE_VOID) {
            unsigned ret_size = codegen_type_size(instruction->dst.type);
            for (unsigned rb = 0; rb < ret_size; rb++) {
              lda_zpg(e, (uint8_t)(RET + rb));
              emit_store_byte(e, &map, &instruction->dst, rb);
            }
          }
          break;
        }

        // -- comparisons & boolean --

        case TAC_NOT: {
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
          unsigned width = codegen_type_size(instruction->src1.type);
          emit_load_byte(e, &map, &instruction->src1, 0);
          if (width > 1)
            emit_ora_byte(e, &map, &instruction->src1, 1);
          // A is now nonzero iff the original value was truthy.
          // Convert to boolean: 0 → 1, nonzero → 0.
          beq_rel(e, 4);       // +4: skip LDA #0 + BEQ +2
          lda_imm(e, 0);
          beq_rel(e, 2);       // +2: skip LDA #1 (always taken, Z=1)
          lda_imm(e, 1);
          sta_zpg(e, dst_addr);
          break;
        }

        case TAC_LT:  case TAC_GTE: case TAC_GT: case TAC_LTE:
        case TAC_EQ:  case TAC_NEQ: {
          uint8_t dst_addr = zp_map_lookup(&map, &instruction->dst);
          tac_operand_t *left, *right;
          uint8_t branch_op;

          switch (instruction->op) {
            case TAC_GT:  left = &instruction->src2; right = &instruction->src1; branch_op = 0x90; break;
            case TAC_LTE: left = &instruction->src2; right = &instruction->src1; branch_op = 0xB0; break;
            case TAC_LT:  left = &instruction->src1; right = &instruction->src2; branch_op = 0x90; break;
            case TAC_GTE: left = &instruction->src1; right = &instruction->src2; branch_op = 0xB0; break;
            case TAC_EQ:  left = &instruction->src1; right = &instruction->src2; branch_op = 0xF0; break;
            case TAC_NEQ: left = &instruction->src1; right = &instruction->src2; branch_op = 0xD0; break;
            default:      left = &instruction->src1; right = &instruction->src2; branch_op = 0x90; break;
          }

          unsigned cmp_width = codegen_type_size(left->type);
          int is_signed = is_signed_type(left->type);
          int is_ordering = (instruction->op != TAC_EQ && instruction->op != TAC_NEQ);

          if (cmp_width == 1 && (!is_signed || !is_ordering)) {
            // u8 unsigned ordering, or u8/i8 EQ/NEQ (sign-agnostic)
            emit_load_byte(e, &map, left, 0);
            emit_cmp_byte(e, &map, right, 0);
            EMIT(branch_op); EMIT(4);
            lda_imm(e, 0);
            EMIT(0xF0); EMIT(2);
            lda_imm(e, 1);
          } else if (cmp_width == 1 && is_signed) {
            // i8 signed ordering: N XOR V pattern
            size_t p_true, p_done;
            emit_load_byte(e, &map, left, 0);
            sec(e);
            emit_sbc_byte(e, &map, right, 0);
            bvc_rel(e, 2);
            eor_imm(e, 0x80);
            // N flag = (left < right)
            EMIT(0x30); p_true = e->code_pos; EMIT(0); // BMI true
            // false:
            lda_imm(e, 0);
            EMIT(0xF0); p_done = e->code_pos; EMIT(0); // BEQ done
            // true:
            e->rom[p_true] = (uint8_t)(e->code_pos - p_true - 1);
            lda_imm(e, 1);
            // done:
            e->rom[p_done] = (uint8_t)(e->code_pos - p_done - 1);
            if (branch_op == 0xB0) {
              eor_imm(e, 0x01);
            }
          } else if (!is_ordering) {
            // u16/i16 EQ/NEQ (sign-agnostic)
            size_t p1, p2, p3;
            if (instruction->op == TAC_EQ) {
              emit_load_byte(e, &map, left, 1);
              emit_cmp_byte(e, &map, right, 1);
              EMIT(0xD0); p1 = e->code_pos; EMIT(0); // BNE false
              emit_load_byte(e, &map, left, 0);
              emit_cmp_byte(e, &map, right, 0);
              EMIT(0xF0); p2 = e->code_pos; EMIT(0); // BEQ true
              // false:
              e->rom[p1] = (uint8_t)(e->code_pos - p1 - 1);
              lda_imm(e, 0);
              EMIT(0xF0); p3 = e->code_pos; EMIT(0); // BEQ done
              // true:
              e->rom[p2] = (uint8_t)(e->code_pos - p2 - 1);
              lda_imm(e, 1);
              // done:
              e->rom[p3] = (uint8_t)(e->code_pos - p3 - 1);
            } else {
              emit_load_byte(e, &map, left, 1);
              emit_cmp_byte(e, &map, right, 1);
              EMIT(0xD0); p1 = e->code_pos; EMIT(0); // BNE true
              emit_load_byte(e, &map, left, 0);
              emit_cmp_byte(e, &map, right, 0);
              EMIT(0xD0); p2 = e->code_pos; EMIT(0); // BNE true
              // false:
              lda_imm(e, 0);
              EMIT(0xF0); p3 = e->code_pos; EMIT(0); // BEQ done
              // true:
              e->rom[p1] = (uint8_t)(e->code_pos - p1 - 1);
              e->rom[p2] = (uint8_t)(e->code_pos - p2 - 1);
              lda_imm(e, 1);
              // done:
              e->rom[p3] = (uint8_t)(e->code_pos - p3 - 1);
            }
          } else if (!is_signed) {
            // u16 unsigned ordering
            size_t p_true1, p_false, p_true2, p_done;
            emit_load_byte(e, &map, left, 1);
            emit_cmp_byte(e, &map, right, 1);
            EMIT(0x90); p_true1 = e->code_pos; EMIT(0);  // BCC true
            EMIT(0xD0); p_false = e->code_pos; EMIT(0);  // BNE false
            emit_load_byte(e, &map, left, 0);
            emit_cmp_byte(e, &map, right, 0);
            EMIT(0x90); p_true2 = e->code_pos; EMIT(0);  // BCC true
            // false:
            e->rom[p_false] = (uint8_t)(e->code_pos - p_false - 1);
            lda_imm(e, 0);
            EMIT(0xF0); p_done = e->code_pos; EMIT(0);   // BEQ done
            // true:
            e->rom[p_true1] = (uint8_t)(e->code_pos - p_true1 - 1);
            e->rom[p_true2] = (uint8_t)(e->code_pos - p_true2 - 1);
            lda_imm(e, 1);
            // done:
            e->rom[p_done] = (uint8_t)(e->code_pos - p_done - 1);
            if (branch_op == 0xB0) {
              eor_imm(e, 0x01);
            }
          } else {
            // i16 signed ordering: N XOR V on high byte, unsigned low byte
            size_t p_low, p_true1, p_skip, p_true2, p_done;
            emit_load_byte(e, &map, left, 1);
            sec(e);
            emit_sbc_byte(e, &map, right, 1);
            EMIT(0xF0); p_low = e->code_pos; EMIT(0);    // BEQ low_compare
            bvc_rel(e, 2);
            eor_imm(e, 0x80);
            EMIT(0x30); p_true1 = e->code_pos; EMIT(0);  // BMI true
            // high bytes differ, not less → skip to false
            EMIT(0x4C); p_skip = e->code_pos; EMIT(0); EMIT(0); // JMP false
            // low_compare:
            e->rom[p_low] = (uint8_t)(e->code_pos - p_low - 1);
            emit_load_byte(e, &map, left, 0);
            emit_cmp_byte(e, &map, right, 0);
            EMIT(0x90); p_true2 = e->code_pos; EMIT(0);  // BCC true
            // false:
            {
              uint16_t false_addr = (uint16_t)(ROM_START + e->code_pos);
              e->rom[p_skip]     = (uint8_t)(false_addr & 0xFF);
              e->rom[p_skip + 1] = (uint8_t)(false_addr >> 8);
            }
            lda_imm(e, 0);
            EMIT(0xF0); p_done = e->code_pos; EMIT(0);   // BEQ done
            // true:
            e->rom[p_true1] = (uint8_t)(e->code_pos - p_true1 - 1);
            e->rom[p_true2] = (uint8_t)(e->code_pos - p_true2 - 1);
            lda_imm(e, 1);
            // done:
            e->rom[p_done] = (uint8_t)(e->code_pos - p_done - 1);
            if (branch_op == 0xB0) {
              eor_imm(e, 0x01);
            }
          }

          sta_zpg(e, dst_addr);
          break;
        }

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

// Load src1/src2 into 8-bit helper slots, JSR, read one result byte.
#define EMIT_ARITH8(ROUTINE, RES_SLOT, NEEDS_FLAG) do {                                   \
  emit_load_byte(e, &map, &instruction->src1, 0); sta_zpg(e, HELPER_ARG1);                \
  emit_load_byte(e, &map, &instruction->src2, 0); sta_zpg(e, HELPER_ARG2);                \
  jsr(e, ROUTINE); e->NEEDS_FLAG = 1;                                                     \
  lda_zpg(e, RES_SLOT); emit_store_byte(e, &map, &instruction->dst, 0);                   \
} while (0)

// Load src1/src2 into 16-bit helper slots, JSR, read two result bytes.
#define EMIT_ARITH16(ROUTINE, RES_SLOT, NEEDS_FLAG) do {                                  \
  emit_load_byte(e, &map, &instruction->src1, 0); sta_zpg(e, HELPER16_ARG1);              \
  emit_load_byte(e, &map, &instruction->src1, 1); sta_zpg(e, (uint8_t)(HELPER16_ARG1+1)); \
  emit_load_byte(e, &map, &instruction->src2, 0); sta_zpg(e, HELPER16_ARG2);              \
  emit_load_byte(e, &map, &instruction->src2, 1); sta_zpg(e, (uint8_t)(HELPER16_ARG2+1)); \
  jsr(e, ROUTINE); e->NEEDS_FLAG = 1;                                                     \
  lda_zpg(e, RES_SLOT); emit_store_byte(e, &map, &instruction->dst, 0);                   \
  lda_zpg(e, (uint8_t)((RES_SLOT)+1)); emit_store_byte(e, &map, &instruction->dst, 1);    \
} while (0)

        case TAC_MUL: {
          if (codegen_type_size(instruction->dst.type) > 1)
            EMIT_ARITH16("__mul16", HELPER16_RES, needs_mul16);
          else
            EMIT_ARITH8("__mul8", HELPER_RES, needs_mul8);
          break;
        }

        case TAC_DIV: {
          int is_s = is_signed_type(instruction->dst.type);
          if (codegen_type_size(instruction->dst.type) > 1) {
            if (is_s) EMIT_ARITH16("__sdiv16", HELPER16_RES, needs_sdiv16);
            else      EMIT_ARITH16("__div16",  HELPER16_RES, needs_div16);
          } else {
            if (is_s) EMIT_ARITH8("__sdiv8", HELPER_RES, needs_sdiv8);
            else      EMIT_ARITH8("__div8",  HELPER_RES, needs_div8);
          }
          break;
        }

        case TAC_MOD: {
          int is_s = is_signed_type(instruction->dst.type);
          if (codegen_type_size(instruction->dst.type) > 1) {
            if (is_s) EMIT_ARITH16("__sdiv16", HELPER16_REM, needs_sdiv16);
            else      EMIT_ARITH16("__div16",  HELPER16_REM, needs_div16);
          } else {
            if (is_s) EMIT_ARITH8("__sdiv8", HELPER_REM, needs_sdiv8);
            else      EMIT_ARITH8("__div8",  HELPER_REM, needs_div8);
          }
          break;
        }

#undef EMIT_ARITH8
#undef EMIT_ARITH16

        case TAC_BAND: {
          unsigned width = codegen_type_size(instruction->dst.type);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_and_byte(e, &map, &instruction->src2, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_BOR: {
          unsigned width = codegen_type_size(instruction->dst.type);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_ora_byte(e, &map, &instruction->src2, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_BXOR: {
          unsigned width = codegen_type_size(instruction->dst.type);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            emit_eor_byte(e, &map, &instruction->src2, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_BNOT: {
          unsigned width = codegen_type_size(instruction->dst.type);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            eor_imm(e, 0xFF);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

        case TAC_SHL: {
          unsigned width = codegen_type_size(instruction->dst.type);
          uint8_t dst_zp = zp_map_lookup(&map, &instruction->dst);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            sta_zpg(e, (uint8_t)(dst_zp + b));
          }
          if (instruction->src2.kind == OPERAND_CONST_INT) {
            unsigned count = (unsigned)instruction->src2.int_val;
            for (unsigned s = 0; s < count; s++) {
              asl_zpg(e, dst_zp);
              if (width > 1) rol_zpg(e, (uint8_t)(dst_zp + 1));
            }
          } else {
            // variable count: loop with X as counter
            // loop body: asl(2) [+rol(2)] + dex(1) + bne(2) = 2*width+3 bytes
            unsigned loop_size = 2u * width + 3u;
            emit_load_byte(e, &map, &instruction->src2, 0);
            beq_rel(e, (uint8_t)(1u + loop_size));  // skip TAX + loop if count==0
            tax(e);
            asl_zpg(e, dst_zp);
            if (width > 1) rol_zpg(e, (uint8_t)(dst_zp + 1));
            dex(e);
            bne_rel(e, (uint8_t)(256u - loop_size));  // back to asl
          }
          break;
        }

        case TAC_SHR: {
          unsigned width = codegen_type_size(instruction->dst.type);
          uint8_t dst_zp = zp_map_lookup(&map, &instruction->dst);
          int is_signed = is_signed_type(instruction->src1.type);
          for (unsigned b = 0; b < width; b++) {
            emit_load_byte(e, &map, &instruction->src1, b);
            sta_zpg(e, (uint8_t)(dst_zp + b));
          }
          if (instruction->src2.kind == OPERAND_CONST_INT) {
            unsigned count = (unsigned)instruction->src2.int_val;
            for (unsigned s = 0; s < count; s++) {
              if (is_signed) {
                // ASR: set carry = sign bit of hi byte, then rotate right
                lda_zpg(e, (uint8_t)(dst_zp + width - 1));
                cmp_imm(e, 0x80);
                ror_zpg(e, (uint8_t)(dst_zp + width - 1));
                if (width > 1) ror_zpg(e, dst_zp);
              } else {
                // LSR: zero-fill MSB
                if (width > 1) lsr_zpg(e, (uint8_t)(dst_zp + 1));
                if (width == 1) lsr_zpg(e, dst_zp);
                else            ror_zpg(e, dst_zp);
              }
            }
          } else {
            if (is_signed) {
              // loop body: lda(2)+cmp(2)+ror hi(2)[+ror lo(2)]+dex(1)+bne(2) = 2*width+7
              unsigned loop_size = 2u * width + 7u;
              emit_load_byte(e, &map, &instruction->src2, 0);
              beq_rel(e, (uint8_t)(1u + loop_size));
              tax(e);
              lda_zpg(e, (uint8_t)(dst_zp + width - 1));
              cmp_imm(e, 0x80);
              ror_zpg(e, (uint8_t)(dst_zp + width - 1));
              if (width > 1) ror_zpg(e, dst_zp);
              dex(e);
              bne_rel(e, (uint8_t)(256u - loop_size));
            } else {
              // loop body: lsr hi(2)[+ror lo(2)]+dex(1)+bne(2) = 2*width+3
              unsigned loop_size = 2u * width + 3u;
              emit_load_byte(e, &map, &instruction->src2, 0);
              beq_rel(e, (uint8_t)(1u + loop_size));
              tax(e);
              if (width > 1) lsr_zpg(e, (uint8_t)(dst_zp + 1));
              if (width == 1) lsr_zpg(e, dst_zp);
              else            ror_zpg(e, dst_zp);
              dex(e);
              bne_rel(e, (uint8_t)(256u - loop_size));
            }
          }
          break;
        }

        case TAC_NEG: {
          unsigned width = codegen_type_size(instruction->dst.type);
          sec(e);
          for (unsigned b = 0; b < width; b++) {
            lda_imm(e, 0);
            emit_sbc_byte(e, &map, &instruction->src1, b);
            emit_store_byte(e, &map, &instruction->dst, b);
          }
          break;
        }

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

        case TAC_FIELD_LOAD: {
          // dst = src1.field  (src1 may be a struct value or a Struct*)
          const char *sname = instruction->src1.type.struct_name;
          ir_field_def_t *field = lookup_struct_field(e, sname, instruction->field_name);
          if (!field) {
            fprintf(stderr, "codegen: unknown field '%s' on struct '%s'\n",
                    instruction->field_name, sname);
            return 0;
          }
          unsigned fsize = full_type_size(e, field->type);

          if (instruction->src1.type.is_ptr) {
            // pointer-to-struct: LDY #(offset+b); LDA ($ptr),Y
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
            for (unsigned b = 0; b < fsize; b++) {
              ldy_imm(e, (uint8_t)(field->offset + b));
              lda_ind_y(e, ptr_zp);
              emit_store_byte(e, &map, &instruction->dst, b);
            }
          } else {
            // struct value: use emit_load_byte at (field->offset + b) so globals get abs
            for (unsigned b = 0; b < fsize; b++) {
              emit_load_byte(e, &map, &instruction->src1, field->offset + b);
              emit_store_byte(e, &map, &instruction->dst, b);
            }
          }
          break;
        }

        case TAC_FIELD_STORE: {
          // dst.field = src1  (dst may be a struct value or a Struct*)
          const char *sname = instruction->dst.type.struct_name;
          ir_field_def_t *field = lookup_struct_field(e, sname, instruction->field_name);
          if (!field) {
            fprintf(stderr, "codegen: unknown field '%s' on struct '%s'\n",
                    instruction->field_name, sname);
            return 0;
          }
          unsigned fsize = full_type_size(e, field->type);

          if (instruction->dst.type.is_ptr) {
            // pointer-to-struct: load value, then STA ($ptr),Y
            uint8_t ptr_zp = zp_map_lookup(&map, &instruction->dst);
            if (instruction->dst.kind == OPERAND_VAR) {
              global_entry_t *g = lookup_global(e, instruction->dst.name);
              if (g) {
                for (unsigned b = 0; b < 2; b++) {
                  lda_abs(e, (uint16_t)(g->ram_addr + b));
                  sta_zpg(e, (uint8_t)(ptr_zp + b));
                }
              }
            }
            for (unsigned b = 0; b < fsize; b++) {
              emit_load_byte(e, &map, &instruction->src1, b);
              ldy_imm(e, (uint8_t)(field->offset + b));
              sta_ind_y(e, ptr_zp);
            }
          } else {
            // struct value: emit_load_byte + emit_store_byte at (field->offset + b)
            for (unsigned b = 0; b < fsize; b++) {
              emit_load_byte(e, &map, &instruction->src1, b);
              emit_store_byte(e, &map, &instruction->dst, field->offset + b);
            }
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
// Arithmetic helper subroutines
// ----------------------------------------------------------------

// u8 multiply: HELPER_ARG1 * HELPER_ARG2 → HELPER_RES (shift-and-add).
// ARG1 and ARG2 are consumed (modified) by the routine.
//
// Loop body layout (16 bytes per iteration):
//   LSR ARG2 (2) | BCC +7 (2) | CLC (1) | LDA RES (2) | ADC ARG1 (2) | STA RES (2)
//   [BCC target:] ASL ARG1 (2) | DEX (1) | BNE -16 (2)
static void emit_mul8_helper(emitter_t *e) {
  register_func_label(e, "__mul8", (uint16_t)(ROM_START + e->code_pos));
  lda_imm(e, 0);
  sta_zpg(e, HELPER_RES);
  ldx_imm(e, 8);
  // loop:
  lsr_zpg(e, HELPER_ARG2);          // LSB of multiplier → carry
  bcc_rel(e, 7);                     // skip add if bit was 0 (7 = CLC+LDA+ADC+STA)
  clc(e);
  lda_zpg(e, HELPER_RES);
  adc_zpg(e, HELPER_ARG1);
  sta_zpg(e, HELPER_RES);
  // BCC target:
  asl_zpg(e, HELPER_ARG1);          // shift multiplicand left
  dex(e);
  bne_rel(e, (uint8_t)(256u - 16u)); // back to LSR
  rts(e);
}


// i8 signed divide: HELPER_ARG1 / HELPER_ARG2 → HELPER_RES (quotient), HELPER_REM (remainder).
// Encodes signs in HELPER_SIGN (bit7=negate quotient, bit6=negate remainder), takes absolute
// values, calls __div8, then restores signs. Follows C truncation-toward-zero convention.
static void emit_sdiv8_helper(emitter_t *e) {
  register_func_label(e, "__sdiv8", (uint16_t)(ROM_START + e->code_pos));
  lda_imm(e, 0);
  sta_zpg(e, HELPER_SIGN);

  // If dividend (ARG1) negative: negate it, set bits 7+6 in SIGN (quotient and remainder both flip).
  lda_zpg(e, HELPER_ARG1);
  bpl_rel(e, 11);        // skip 11 bytes if positive:
  sec(e);                //   SEC
  lda_imm(e, 0);         //   LDA #0
  sbc_zpg(e, HELPER_ARG1); // SBC ARG1
  sta_zpg(e, HELPER_ARG1); // STA ARG1  (= -ARG1)
  lda_imm(e, 0xC0);     //   LDA #$C0
  sta_zpg(e, HELPER_SIGN); // STA SIGN  (bits 7+6)

  // If divisor (ARG2) negative: negate it, toggle bit 7 in SIGN (quotient sign flips again).
  lda_zpg(e, HELPER_ARG2);
  bpl_rel(e, 13);        // skip 13 bytes if positive:
  sec(e);                //   SEC
  lda_imm(e, 0);         //   LDA #0
  sbc_zpg(e, HELPER_ARG2); // SBC ARG2
  sta_zpg(e, HELPER_ARG2); // STA ARG2  (= -ARG2)
  lda_zpg(e, HELPER_SIGN); // LDA SIGN
  eor_imm(e, 0x80);     //   EOR #$80   (toggle bit 7)
  sta_zpg(e, HELPER_SIGN); // STA SIGN

  jsr(e, "__div8");

  // Negate quotient if bit 7 of SIGN is set.
  lda_zpg(e, HELPER_SIGN);
  bpl_rel(e, 7);         // skip 7 bytes if bit 7 = 0:
  sec(e);                //   SEC
  lda_imm(e, 0);         //   LDA #0
  sbc_zpg(e, HELPER_RES); //  SBC RES
  sta_zpg(e, HELPER_RES); //  STA RES

  // Negate remainder if bit 6 of SIGN is set.
  lda_zpg(e, HELPER_SIGN);
  and_imm(e, 0x40);
  beq_rel(e, 7);         // skip 7 bytes if bit 6 = 0:
  sec(e);                //   SEC
  lda_imm(e, 0);         //   LDA #0
  sbc_zpg(e, HELPER_REM); //  SBC REM
  sta_zpg(e, HELPER_REM); //  STA REM

  rts(e);
}


// u8 divide: HELPER_ARG1 / HELPER_ARG2 → HELPER_RES (quotient), HELPER_REM (remainder).
// Uses binary long division (shift-subtract). ARG1 is consumed.
//
// Loop body layout (18 bytes per iteration):
//   ASL ARG1 (2) | ROL REM (2) | LDA REM (2) | SEC (1) | SBC ARG2 (2) | BCC +2 (2)
//   | STA REM (2) | [BCC target:] ROL RES (2) | DEX (1) | BNE -18 (2)
static void emit_div8_helper(emitter_t *e) {
  register_func_label(e, "__div8", (uint16_t)(ROM_START + e->code_pos));
  lda_imm(e, 0);
  sta_zpg(e, HELPER_REM);
  sta_zpg(e, HELPER_RES);
  ldx_imm(e, 8);
  // loop:
  asl_zpg(e, HELPER_ARG1);          // shift dividend left, MSB → carry
  rol_zpg(e, HELPER_REM);           // remainder = (rem << 1) | carry
  lda_zpg(e, HELPER_REM);
  sec(e);
  sbc_zpg(e, HELPER_ARG2);          // try to subtract divisor
  bcc_rel(e, 2);                     // if borrow (didn't fit), skip STA
  sta_zpg(e, HELPER_REM);           // commit: remainder -= divisor
  // BCC target:
  rol_zpg(e, HELPER_RES);           // quotient bit = carry (1=fit, 0=didn't)
  dex(e);
  bne_rel(e, (uint8_t)(256u - 18u)); // back to ASL
  rts(e);
}


// u16 multiply: HELPER16_ARG1 * HELPER16_ARG2 → HELPER16_RES (shift-and-add).
// Both ARG operands are consumed. Correct for signed i16 (low 16 bits are sign-agnostic).
//
// Loop body layout (26 bytes per iteration):
//   LSR ARG2_HI (2) | ROR ARG2_LO (2) | BCC +13 (2) | CLC (1) | LDA RES_LO (2)
//   | ADC ARG1_LO (2) | STA RES_LO (2) | LDA RES_HI (2) | ADC ARG1_HI (2)
//   | STA RES_HI (2) [BCC target:] | ASL ARG1_LO (2) | ROL ARG1_HI (2)
//   | DEX (1) | BNE -26 (2)
static void emit_mul16_helper(emitter_t *e) {
  register_func_label(e, "__mul16", (uint16_t)(ROM_START + e->code_pos));
  lda_imm(e, 0);
  sta_zpg(e, HELPER16_RES);
  sta_zpg(e, (uint8_t)(HELPER16_RES + 1));
  ldx_imm(e, 16);
  // loop:
  lsr_zpg(e, (uint8_t)(HELPER16_ARG2 + 1)); // shift multiplier right, LSB of full 16-bit → carry
  ror_zpg(e, HELPER16_ARG2);
  bcc_rel(e, 13);                             // skip add if LSB was 0
  clc(e);
  lda_zpg(e, HELPER16_RES);
  adc_zpg(e, HELPER16_ARG1);
  sta_zpg(e, HELPER16_RES);
  lda_zpg(e, (uint8_t)(HELPER16_RES + 1));
  adc_zpg(e, (uint8_t)(HELPER16_ARG1 + 1));
  sta_zpg(e, (uint8_t)(HELPER16_RES + 1));
  // no_add (BCC target, byte 19 from loop start):
  asl_zpg(e, HELPER16_ARG1);
  rol_zpg(e, (uint8_t)(HELPER16_ARG1 + 1));
  dex(e);
  bne_rel(e, (uint8_t)(256u - 26u));          // back to LSR
  rts(e);
}


// u16 divide: HELPER16_ARG1 / HELPER16_ARG2 → HELPER16_RES (quotient), HELPER16_REM (remainder).
// Uses CMP-based comparison to correctly handle divisors with bit 15 set (≥ $8000).
// ARG1 is consumed. Sets carry correctly for ROL quotient: 1=subtracted, 0=skipped.
//
// Loop body layout (45 bytes per iteration):
//   ASL ARG1 (2) | ROL ARG1_HI (2) | ROL REM (2) | ROL REM_HI (2) | BCS dosub+14 (2)
//   | LDA REM_HI (2) | CMP ARG2_HI (2) | BCC nosub+22 (2) | BNE dosub+6 (2)
//   | LDA REM (2) | CMP ARG2 (2) | BCC nosub+14 (2)
//   [dosub:] SEC (1) | LDA REM (2) | SBC ARG2 (2) | STA REM (2) | LDA REM_HI (2)
//   | SBC ARG2_HI (2) | STA REM_HI (2) | SEC (1)
//   [nosub:] ROL RES (2) | ROL RES_HI (2) | DEX (1) | BNE -45 (2)
static void emit_div16_helper(emitter_t *e) {
  register_func_label(e, "__div16", (uint16_t)(ROM_START + e->code_pos));
  lda_imm(e, 0);
  sta_zpg(e, HELPER16_REM);
  sta_zpg(e, (uint8_t)(HELPER16_REM + 1));
  sta_zpg(e, HELPER16_RES);
  sta_zpg(e, (uint8_t)(HELPER16_RES + 1));
  ldx_imm(e, 16);
  // loop:
  asl_zpg(e, HELPER16_ARG1);
  rol_zpg(e, (uint8_t)(HELPER16_ARG1 + 1));
  rol_zpg(e, HELPER16_REM);
  rol_zpg(e, (uint8_t)(HELPER16_REM + 1));   // carry = 17th-bit overflow (REM ≥ $10000)
  bcs_rel(e, 14);                              // overflow → must subtract regardless of compare
  lda_zpg(e, (uint8_t)(HELPER16_REM + 1));
  cmp_zpg(e, (uint8_t)(HELPER16_ARG2 + 1));
  bcc_rel(e, 22);                              // REM_HI < ARG2_HI → no subtract
  bne_rel(e, 6);                               // REM_HI > ARG2_HI → subtract
  lda_zpg(e, HELPER16_REM);
  cmp_zpg(e, HELPER16_ARG2);
  bcc_rel(e, 14);                              // REM_LO < ARG2_LO (hi equal) → no subtract
  // dosub (byte 24 from loop start):
  sec(e);
  lda_zpg(e, HELPER16_REM);
  sbc_zpg(e, HELPER16_ARG2);
  sta_zpg(e, HELPER16_REM);
  lda_zpg(e, (uint8_t)(HELPER16_REM + 1));
  sbc_zpg(e, (uint8_t)(HELPER16_ARG2 + 1));
  sta_zpg(e, (uint8_t)(HELPER16_REM + 1));
  sec(e);                                      // carry = 1: quotient bit = 1
  // nosub (byte 38 from loop start):
  rol_zpg(e, HELPER16_RES);
  rol_zpg(e, (uint8_t)(HELPER16_RES + 1));
  dex(e);
  bne_rel(e, (uint8_t)(256u - 45u));           // back to ASL
  rts(e);
}


// i16 signed divide: HELPER16_ARG1 / HELPER16_ARG2 → HELPER16_RES, HELPER16_REM.
// Records sign in HELPER_SIGN (bit7=negate quotient, bit6=negate remainder),
// takes absolute values, calls __div16, then restores signs. Truncates toward zero.
static void emit_sdiv16_helper(emitter_t *e) {
  register_func_label(e, "__sdiv16", (uint16_t)(ROM_START + e->code_pos));
  lda_imm(e, 0);
  sta_zpg(e, HELPER_SIGN);

  // If ARG1 negative: negate it, set bits 7+6 in SIGN (both quotient and remainder flip).
  lda_zpg(e, (uint8_t)(HELPER16_ARG1 + 1));
  bpl_rel(e, 17);                              // skip 17 bytes if positive
  sec(e);
  lda_imm(e, 0);
  sbc_zpg(e, HELPER16_ARG1);
  sta_zpg(e, HELPER16_ARG1);
  lda_imm(e, 0);
  sbc_zpg(e, (uint8_t)(HELPER16_ARG1 + 1));
  sta_zpg(e, (uint8_t)(HELPER16_ARG1 + 1));
  lda_imm(e, 0xC0);
  sta_zpg(e, HELPER_SIGN);
  // pos_arg1 (byte 25 from function start):

  // If ARG2 negative: negate it, toggle bit 7 in SIGN (quotient sign flips again).
  lda_zpg(e, (uint8_t)(HELPER16_ARG2 + 1));
  bpl_rel(e, 19);                              // skip 19 bytes if positive
  sec(e);
  lda_imm(e, 0);
  sbc_zpg(e, HELPER16_ARG2);
  sta_zpg(e, HELPER16_ARG2);
  lda_imm(e, 0);
  sbc_zpg(e, (uint8_t)(HELPER16_ARG2 + 1));
  sta_zpg(e, (uint8_t)(HELPER16_ARG2 + 1));
  lda_zpg(e, HELPER_SIGN);
  eor_imm(e, 0x80);
  sta_zpg(e, HELPER_SIGN);
  // pos_arg2 (byte 48 from function start):

  jsr(e, "__div16");

  // Negate quotient if bit 7 of SIGN set.
  lda_zpg(e, HELPER_SIGN);
  bpl_rel(e, 13);                              // skip 13 bytes if bit 7 = 0
  sec(e);
  lda_imm(e, 0);
  sbc_zpg(e, HELPER16_RES);
  sta_zpg(e, HELPER16_RES);
  lda_imm(e, 0);
  sbc_zpg(e, (uint8_t)(HELPER16_RES + 1));
  sta_zpg(e, (uint8_t)(HELPER16_RES + 1));
  // pos_res (byte 68 from function start):

  // Negate remainder if bit 6 of SIGN set.
  lda_zpg(e, HELPER_SIGN);
  and_imm(e, 0x40);
  beq_rel(e, 13);                              // skip 13 bytes if bit 6 = 0
  sec(e);
  lda_imm(e, 0);
  sbc_zpg(e, HELPER16_REM);
  sta_zpg(e, HELPER16_REM);
  lda_imm(e, 0);
  sbc_zpg(e, (uint8_t)(HELPER16_REM + 1));
  sta_zpg(e, (uint8_t)(HELPER16_REM + 1));
  // pos_rem (byte 87 from function start):

  rts(e);
}


// ROM footer layout (offsets from ROM_START):
//   $FFF6-$FFF7  SYMTABLE_START_PTR:   little-endian absolute address of the "C02S" symbol
//                table header, or $EAEA (NOP fill) if no table was written. The disassembler
//                reads this first; old binaries that lack a table have $EAEA here, which is
//                in range but fails the magic-byte check, so they degrade gracefully.
//   $FFF8-$FFF9  SYMTABLE_BOUNDARY_PTR: little-endian absolute address of the first byte
//                past the code+data region (= start of NOP fill). Used by the disassembler
//                to know where executable/data bytes end and padding begins.
//   $FFFA-$FFFF  NMI / Reset / IRQ vectors (written by emit_vectors).
#define SYMTABLE_START_PTR    (0xFFF6 - ROM_START)
#define SYMTABLE_BOUNDARY_PTR (0xFFF8 - ROM_START)

// Write the "C02S" symbol table into the NOP fill area immediately after the data section,
// then store its absolute address at SYMTABLE_START_PTR so the disassembler can find it.
// Each entry is: u16 address (LE) + null-terminated name. The table is silently omitted if
// the code+data section is too large to fit it before the footer (extremely unlikely in
// practice — a fully packed 32KB image still leaves the footer region intact).
static void emit_symbol_table(emitter_t *e) {
  size_t sym_size = 6; // "C02S" (4) + count u16 (2)
  for (unsigned i = 0; i < e->func_label_count; i++)
    sym_size += 2 + strlen(e->func_labels[i].name) + 1;
  
  if (e->code_pos + sym_size <= SYMTABLE_START_PTR) {
    uint16_t symtab_addr = (uint16_t)(ROM_START + e->code_pos);
    uint8_t *p = e->rom + e->code_pos;
    
    // magic number then number of labels
    *p++ = 'C'; *p++ = '0'; *p++ = '2'; *p++ = 'S';
    *p++ = (uint8_t)(e->func_label_count & 0xFF);
    *p++ = (uint8_t)(e->func_label_count >> 8);

    for (unsigned i = 0; i < e->func_label_count; i++) {
      uint16_t addr = e->func_labels[i].addr;
      *p++ = (uint8_t)(addr & 0xFF);
      *p++ = (uint8_t)(addr >> 8);
      size_t len = strlen(e->func_labels[i].name);
      memcpy(p, e->func_labels[i].name, len + 1);
      p += len + 1;
    }

    e->rom[SYMTABLE_START_PTR]     = (uint8_t)(symtab_addr & 0xFF);
    e->rom[SYMTABLE_START_PTR + 1] = (uint8_t)(symtab_addr >> 8);
  }
} 


// ----------------------------------------------------------------
// Main code gen
// ----------------------------------------------------------------

static void emitter_free(emitter_t *e) {
  arena_free(&e->arena);
}


uint8_t *generate_rom(ir_gen_t *gen, size_t *final_rom_size, int emit_symbols) {
  emitter_t e = { 0 };

  if (!arena_init(&e.arena, 4096)) return NULL;

  e.rom = malloc(ROM_SIZE);
  if (!e.rom) { arena_free(&e.arena); return NULL; }

  memset(e.rom, 0xEA, ROM_SIZE);
  *final_rom_size = ROM_SIZE;

  e.gen = gen;
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

  // add helpers
  if (e.needs_mul8) emit_mul8_helper(&e);
  if (e.needs_sdiv8) { emit_sdiv8_helper(&e); e.needs_div8 = 1; }
  if (e.needs_div8) emit_div8_helper(&e);

  if (e.needs_mul16) emit_mul16_helper(&e);
  if (e.needs_sdiv16) { emit_sdiv16_helper(&e); e.needs_div16 = 1; }
  if (e.needs_div16) emit_div16_helper(&e);

  if (!resolve_func_fixups(&e)) {
    free(e.rom);
    emitter_free(&e);
    *final_rom_size = 0;
    return NULL;
  }

  emit_data_section(&e, gen);

  if (emit_symbols && e.func_label_count > 0) {
    emit_symbol_table(&e);
  }

  uint16_t code_end_addr = (uint16_t)(ROM_START + e.data_pos);
  e.rom[SYMTABLE_BOUNDARY_PTR]     = (uint8_t)(code_end_addr & 0xFF);
  e.rom[SYMTABLE_BOUNDARY_PTR + 1] = (uint8_t)(code_end_addr >> 8);

  emit_vectors(&e);
  emitter_free(&e);

  return e.rom;
}