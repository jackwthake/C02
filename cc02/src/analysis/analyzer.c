#include "analyzer.h"

#include <assert.h>


#define SCOPE_STACK_INITIAL_CAPACITY 8   // matches the starting capacity used by parser.c's scratch buffers


void scope_stack_init(scope_stack_t *stack, arena_t *arena) {
  stack->arena = arena;
  stack->capacity = SCOPE_STACK_INITIAL_CAPACITY;
  stack->scopes = arena_alloc(arena, sizeof(symtab_t) * stack->capacity);
  stack->depth = 0;

  // global scope lives at index 0 from the start
  scope_push(stack);
}


void scope_push(scope_stack_t *stack) {
  if (stack->depth == stack->capacity) {
    unsigned new_capacity = stack->capacity * 2;
    symtab_t *grown = arena_alloc(stack->arena, sizeof(symtab_t) * new_capacity);

    // arena_alloc zeroes new memory but doesn't know about the old
    // allocation - copy the live scopes across by hand
    for (unsigned i = 0; i < stack->depth; i++) {
      grown[i] = stack->scopes[i];
    }

    stack->scopes = grown;
    stack->capacity = new_capacity;
  }

  symtab_init(&stack->scopes[stack->depth]);
  stack->depth++;
}


void scope_pop(scope_stack_t *stack) {
  assert(stack->depth > 1 && "scope_pop: attempted to pop the global scope");
  stack->depth--;
}


int scope_insert_local(scope_stack_t *stack, char *key, symbol_t value) {
  symtab_t *current = &stack->scopes[stack->depth - 1];
  return symtab_insert(current, key, value, stack->arena);
}


symbol_t *scope_lookup(scope_stack_t *stack, const char *key) {
  for (unsigned i = stack->depth; i > 0; i--) {
    symbol_t *found = symtab_lookup(&stack->scopes[i - 1], key);
    if (found) {
      return found;
    }
  }
  return NULL;
}


symtab_t *scope_global(scope_stack_t *stack) {
  return &stack->scopes[0];
}