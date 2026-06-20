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
 * scope_lookup() walks the stack top-down (innermost scope first, down to
 * global at index 0) - this replaces the old Rust SymbolTable's recursive
 * `parent` chain with a plain loop over a flat array.
 */

#include "arena.h"
#include "symtab.h"
#include "parser.h"


typedef struct {
  symtab_t *scopes;     // arena-allocated, grows as needed
  unsigned depth;        // number of scopes currently pushed (>= 1 once initialised - index 0 is global)
  unsigned capacity;     // allocated slots in `scopes`
  arena_t *arena;        // backing arena for both scope growth and symtab entries
} scope_stack_t;


// Initialises `stack` with one scope already pushed (the global scope, at
// index 0) and a starting capacity, all allocated from `arena`. `arena`
// is retained (not copied) and must outlive the stack.
void scope_stack_init(scope_stack_t *stack, arena_t *arena);

// Pushes a new, empty scope onto the stack (growing the backing array if
// needed). The new scope becomes the target of scope_insert_local() until
// the next push or pop.
void scope_push(scope_stack_t *stack);

// Pops the innermost scope off the stack. Asserts depth > 1 - popping the
// global scope (index 0) is a compiler-internal bug, not a user-facing
// error, mirroring how the old analyzer treated "exit global scope" as
// unreachable rather than a normal failure path.
void scope_pop(scope_stack_t *stack);

// Inserts `value` under `key` into the innermost (current) scope only.
// Returns 1 on success, 0 if `key` is already declared in *this* scope
// specifically (shadowing an outer scope's name is allowed - only
// redeclaration within the same scope is rejected).
int scope_insert_local(scope_stack_t *stack, char *key, symbol_t value);

// Looks up `key` starting at the innermost scope and walking outward to
// global. Returns the first match, or NULL if `key` is undeclared
// anywhere visible.
symbol_t *scope_lookup(scope_stack_t *stack, const char *key);

// Convenience accessor for the global scope (index 0) specifically - used
// by pass 1, which only ever registers into the global table and never
// deals with the stack's push/pop behaviour.
symtab_t *scope_global(scope_stack_t *stack);

#endif // __ANALYZER_H__