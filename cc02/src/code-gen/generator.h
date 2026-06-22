#ifndef __GENERATOR_H__
#define __GENERATOR_H__

#include <stddef.h>
#include <stdint.h>

#include "ir.h"

typedef struct {
  char     *name;
  uint16_t  addr;
} func_label_t;

typedef struct {
  size_t    patch_pos;
  unsigned  label_id;
  char     *func_name;
} fixup_t;

typedef struct {
  uint8_t *rom;
  size_t   rom_size;
  size_t   code_pos;
  size_t   data_pos;
  uint16_t ram_start;
  uint16_t zp_next;

  func_label_t *func_labels;
  unsigned      func_label_count;
  unsigned      func_label_capacity;

  fixup_t *fixups;
  unsigned fixup_count;
  unsigned fixup_capacity;

  uint16_t *local_labels;
  unsigned  local_label_count;

  fixup_t *local_fixups;
  unsigned local_fixup_count;
  unsigned local_fixup_capacity;
} emitter_t;

uint8_t *generate_rom(ir_gen_t *gen, size_t *final_rom_size);

#endif
