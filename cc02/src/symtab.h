#ifndef __SYMTAB_H__
#define __SYMTAB_H__

/*
 * symtab.h - symbol table (hash map of name -> symbol_t)
 * ---------------------------------------------------------
 * A fixed-size (SYMTAB_NUM_BUCKETS), chained hash map. Keys are char* that
 * point into the token array (same convention as node_t string fields) -
 * the map never copies or owns key strings, just compares them with
 * strcmp(). Entries are bump-allocated from an arena_t supplied by the
 * caller, so the whole table (and every entry in it) is freed in one shot
 * by freeing that arena - there is no per-entry free().
 *
 * This single symtab_t shape is reused for two different jobs:
 *   - a scope's local variables/params (one per nested block/function)
 *   - the struct registry (name -> SYMBOL_STRUCT), built in pass 1
 * Both just need "name -> symbol_t", so one map type covers both.
 *
 * USAGE
 * -----
 *   arena_t arena;
 *   arena_init(&arena, SOME_CHUNK_SIZE);
 *
 *   symtab_t table;
 *   symtab_init(&table);
 *
 *   symbol_t sym = { .kind = SYMBOL_VARIABLE, ... };
 *   symtab_insert(&table, "x", sym, &arena);   // false if "x" already present
 *
 *   symbol_t *found = symtab_lookup(&table, "x");  // NULL if absent
 */

#include "arena.h"
#include "parser.h"   // type_t, param_list_t, field_list_t

#define SYMTAB_NUM_BUCKETS 16   // power of two - see notes in symtab.c


// ----------------------------------------------------------------
// Symbol
// ----------------------------------------------------------------

typedef enum {
  SYMBOL_VARIABLE,   // covers locals, params, globals, AND registers
  SYMBOL_FUNCTION,
  SYMBOL_STRUCT,
} symbol_kind_t;

typedef struct {
  char *name;             // points into the token array, not owned
  symbol_kind_t kind;
  union {
    struct {
      type_t type;
      unsigned is_register;
      unsigned long addr;       // only meaningful when is_register
    } variable;                 // SYMBOL_VARIABLE

    struct {
      param_list_t params;
      type_t return_type;
    } function;                 // SYMBOL_FUNCTION

    struct {
      field_list_t fields;
    } struct_decl;              // SYMBOL_STRUCT
  };
} symbol_t;


// ----------------------------------------------------------------
// Hash map
// ----------------------------------------------------------------

typedef struct symtab_entry_t {
  char *key;                    // not owned - points into the token array
  symbol_t value;
  struct symtab_entry_t *next;  // chain link, NULL = end of bucket
} symtab_entry_t;

typedef struct {
  symtab_entry_t *buckets[SYMTAB_NUM_BUCKETS];
} symtab_t;


// Zeroes out a table (all buckets empty). No allocation happens here -
// the table itself is just an array of NULL pointers until inserted into.
void symtab_init(symtab_t *table);

// Inserts `value` under `key` into `table`, allocating the chain entry from
// `arena`. Returns 1 on success, 0 if `key` already exists in this table
// (the existing entry is left untouched - this is a redeclaration check,
// not an overwrite). Does NOT search parent scopes - that's the analyzer's
// job (see scope stack notes), this map only knows about its own buckets.
int symtab_insert(symtab_t *table, char *key, symbol_t value, arena_t *arena);

// Looks up `key` in `table` only (no parent-scope walking). Returns a
// pointer to the stored symbol_t, or NULL if not found. The returned
// pointer is valid as long as the backing arena is alive.
symbol_t *symtab_lookup(symtab_t *table, const char *key);

#endif // __SYMTAB_H__