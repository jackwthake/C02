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
OP_EMITTER_SINGLE_ARG(ldx_imm, 0xA2)
OP_EMITTER_SINGLE_ARG(sta_zpg, 0x85)

OP_EMITTER_NO_ARG(txs, 0x9A)
OP_EMITTER_NO_ARG(rts, 0x60)

OP_EMITTER_ABS(jmp_abs, 0x4C)


static void add_fixup(emitter_t *e, char *func_name);
static void jsr(emitter_t *e, char *func_name) {
  EMIT(0x20);
  add_fixup(e, func_name);
  EMIT(0x00);
  EMIT(0x00);
}


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
  (void)gen;
  emitter_t e = { 0 };

  e.rom = malloc(ROM_SIZE);
  if (!e.rom) return NULL;

  memset(e.rom, 0xEA, ROM_SIZE);
  *final_rom_size = ROM_SIZE;

  e.ram_start = RAM_START;
  e.zp_next = REG_START;

  emit_runtime(&e);

  // TODO: emit function bodies from IR
  // For now, emit a stub main that just returns (RTS)
  register_func_label(&e, "main", (uint16_t)(ROM_START + e.code_pos));
  rts(&e);

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