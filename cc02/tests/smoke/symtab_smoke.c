#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "arena.h"
#include "symtab.h"

int main(void) {
  arena_t arena;
  assert(arena_init(&arena, 4096));

  symtab_t table;
  symtab_init(&table);

  // insert a plain variable
  symbol_t a = { .name = "a", .kind = SYMBOL_VARIABLE };
  a.variable.type.kind = TYPE_U8;
  assert(symtab_insert(&table, "a", a, &arena) == 1);

  // insert a register
  symbol_t reg = { .name = "PORTB", .kind = SYMBOL_VARIABLE };
  reg.variable.type.kind = TYPE_U8;
  reg.variable.is_register = 1;
  reg.variable.addr = 0x6000;
  assert(symtab_insert(&table, "PORTB", reg, &arena) == 1);

  // duplicate insert should fail
  assert(symtab_insert(&table, "a", a, &arena) == 0);

  // lookups
  symbol_t *found_a = symtab_lookup(&table, "a");
  assert(found_a != NULL);
  assert(found_a->kind == SYMBOL_VARIABLE);
  assert(found_a->variable.type.kind == TYPE_U8);
  assert(found_a->variable.is_register == 0);

  symbol_t *found_reg = symtab_lookup(&table, "PORTB");
  assert(found_reg != NULL);
  assert(found_reg->variable.is_register == 1);
  assert(found_reg->variable.addr == 0x6000);

  // missing key
  assert(symtab_lookup(&table, "nonexistent") == NULL);

  // force enough entries to guarantee at least one bucket collision,
  // exercising the chain-walk path in both insert and lookup
  char names[40][8];
  for (int i = 0; i < 40; i++) {
    snprintf(names[i], sizeof(names[i]), "v%d", i);
    symbol_t v = { .name = names[i], .kind = SYMBOL_VARIABLE };
    v.variable.type.kind = TYPE_U16;
    assert(symtab_insert(&table, names[i], v, &arena) == 1);
  }
  for (int i = 0; i < 40; i++) {
    symbol_t *f = symtab_lookup(&table, names[i]);
    assert(f != NULL);
    assert(f->variable.type.kind == TYPE_U16);
  }

  arena_free(&arena);

  printf("all symtab smoke tests passed\n");
  return 0;
}