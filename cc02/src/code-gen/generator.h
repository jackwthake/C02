#ifndef __GENERATOR_H__
#define __GENERATOR_H__

#include <unistd.h>
#include <stdint.h>

#include "ir.h"

typedef struct {
  uint8_t *rom;        // the full ROM buffer
  size_t   rom_size;
  size_t   code_pos;   // grows forward from rom_start
  size_t   data_pos;   // grows forward after code
  uint16_t ram_start;  // where .data gets copied to at runtime
  uint16_t zp_next;    // next free zero-page slot
} emitter_t;

uint8_t *generate_rom(ir_gen_t *gen, size_t *final_rom_size);

#endif
