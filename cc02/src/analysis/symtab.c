#include "symtab.h"

#include <string.h>


/*
 * djb2 string hash - small, fast, well-distributed for short identifier-
 * length strings. Not cryptographic and not meant to be: these are source
 * identifiers, not adversarial input. 5381 and 33 are the well-known djb2
 * constants, not derived from anything specific to this codebase.
 */
static unsigned long hash_str(const char *str) {
  unsigned long hash = 5381;
  int c;
  while ((c = (unsigned char)*str++)) {
    hash = hash * 33 + (unsigned long)c;
  }
  return hash;
}


// SYMTAB_NUM_BUCKETS is a power of two, so `& (SYMTAB_NUM_BUCKETS - 1)` is
// equivalent to `% SYMTAB_NUM_BUCKETS` but cheaper (no division).
static unsigned bucket_index(const char *key) {
  return (unsigned)(hash_str(key) & (SYMTAB_NUM_BUCKETS - 1));
}


void symtab_init(symtab_t *table) {
  memset(table->buckets, 0, sizeof(table->buckets));
}


int symtab_insert(symtab_t *table, char *key, symbol_t value, arena_t *arena) {
  unsigned idx = bucket_index(key);

  // walk the existing chain in this bucket - redeclaration check
  for (symtab_entry_t *e = table->buckets[idx]; e; e = e->next) {
    if (strcmp(e->key, key) == 0) {
      return 0; // already present - caller decides how to report this
    }
  }

  symtab_entry_t *entry = ARENA_ALLOC(arena, symtab_entry_t);
  entry->key = key;
  entry->value = value;

  // push onto the front of this bucket's chain
  entry->next = table->buckets[idx];
  table->buckets[idx] = entry;

  return 1;
}


symbol_t *symtab_lookup(symtab_t *table, const char *key) {
  unsigned idx = bucket_index(key);

  for (symtab_entry_t *e = table->buckets[idx]; e; e = e->next) {
    if (strcmp(e->key, key) == 0) {
      return &e->value;
    }
  }

  return NULL;
}


unsigned is_symtab_empty(symtab_t *s) {
  if (s) {
    for (unsigned i = 0; i < SYMTAB_NUM_BUCKETS; ++i) {
      if (s->buckets[i]) return 0;
    }

    return 1;
  }

  return 0;
}