#ifndef __ANALYZER_H__
#define __ANALYZER_H__

/*
 * analyzer.h - semantic analysis
 * --------------------------------
 * Walks the AST produced by parser.c and checks it for semantic errors:
 * undeclared identifiers, type mismatches, redeclarations, wrong argument
 * counts, etc. Symbol storage (the hash map itself, symbol_t's shape) lives
 * in symtab.h/symtab.c - this file owns the analysis logic and the scope
 * stack that sits on top of that storage.
 *
 * SCOPE STACK
 * -----------
 * A translation unit has one global symtab_t (functions, registers, global
 * vars, struct decls) plus a new symtab_t pushed for every nested block
 * (function body, if/while/for body). scope_stack_t is a growable,
 * arena-backed array of symtab_t - growable so there's no hardcoded nesting
 * depth limit, arena-backed so popping a scope is just decrementing a
 * count (the popped scope's entries stay harmlessly dead in the arena
 * until the whole analysis arena is freed at the end).
 *
 * analyzer_lookup() walks the stack top-down (innermost scope first, down
 * to global at index 0) - this replaces the old Rust SymbolTable's
 * recursive `parent` chain with a plain loop over a flat array.
 */

#include "arena.h"
#include "symtab.h"
#include "parser.h"


typedef struct {
  symtab_t *scopes;     // arena-allocated, grows as needed
  unsigned depth;       // number of scopes currently pushed (>= 1 once initialised - index 0 is global)
  unsigned capacity;    // allocated slots in `scopes`
  arena_t arena;        // backing arena for both scope growth and symtab entries
  unsigned has_errored;
} analyzer_t;

#define ANALYZER_SCOPE_ALLOC_SIZE sizeof(symtab_entry_t) * 64

// Initialises `a` with its own arena and one scope already pushed (the
// global scope, at index 0). Returns 1 on success, 0 if the arena
// allocation fails.
int analyzer_init(analyzer_t *a);

// Frees the backing arena (and with it every scope and symbol entry).
void analyzer_free(analyzer_t *a);

// Pushes a new, empty scope onto the stack (growing the backing array if
// needed). The new scope becomes the target of analyzer_insert_local()
// until the next push or pop.
void analyzer_scope_push(analyzer_t *a);

// Pops the innermost scope off the stack. Asserts depth > 1 - popping the
// global scope (index 0) is a compiler-internal bug.
void analyzer_scope_pop(analyzer_t *a);

// Inserts `value` under `key` into the innermost (current) scope only.
// Returns 1 on success, 0 if `key` is already declared in *this* scope
// (shadowing an outer scope is allowed - only redeclaration within the
// same scope is rejected).
int analyzer_insert_local(analyzer_t *a, char *key, symbol_t value);

// Looks up `key` starting at the innermost scope and walking outward to
// global. Returns the first match, or NULL if undeclared anywhere visible.
symbol_t *analyzer_lookup(analyzer_t *a, const char *key);

// Convenience accessor for the global scope (index 0).
symtab_t *analyzer_global_scope(analyzer_t *a);

symtab_t *analyze(analyzer_t *a, ast_t ast);

// Prints every symbol in `table` to stdout, one per line.
void print_symtab(symtab_t *table);

#endif // __ANALYZER_H__