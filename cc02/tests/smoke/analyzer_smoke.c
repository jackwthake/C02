#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "arena.h"
#include "analyzer.h"

int main(void) {
  arena_t arena;
  assert(arena_init(&arena, 4096));

  scope_stack_t stack;
  scope_stack_init(&stack, &arena);
  assert(stack.depth == 1); // just global

  // insert into global
  symbol_t g = { .name = "g", .kind = SYMBOL_VARIABLE };
  g.variable.type.kind = TYPE_U8;
  assert(scope_insert_local(&stack, "g", g) == 1);

  // push a function scope, insert a param
  scope_push(&stack);
  symbol_t p = { .name = "p", .kind = SYMBOL_VARIABLE };
  p.variable.type.kind = TYPE_U16;
  assert(scope_insert_local(&stack, "p", p) == 1);

  // lookup from inner scope sees both inner and outer
  assert(scope_lookup(&stack, "p") != NULL);
  assert(scope_lookup(&stack, "g") != NULL);
  assert(scope_lookup(&stack, "g")->variable.type.kind == TYPE_U8);

  // shadowing: same name in a nested scope is allowed, and inner wins
  scope_push(&stack);
  symbol_t shadow = { .name = "g", .kind = SYMBOL_VARIABLE };
  shadow.variable.type.kind = TYPE_I16;
  assert(scope_insert_local(&stack, "g", shadow) == 1); // allowed - different scope than outer "g"
  assert(scope_lookup(&stack, "g")->variable.type.kind == TYPE_I16); // inner wins
  scope_pop(&stack);
  assert(scope_lookup(&stack, "g")->variable.type.kind == TYPE_U8); // back to outer

  // redeclaration within the SAME scope is rejected
  symbol_t dup = { .name = "p", .kind = SYMBOL_VARIABLE };
  dup.variable.type.kind = TYPE_U8;
  assert(scope_insert_local(&stack, "p", dup) == 0);

  scope_pop(&stack); // back to just global
  assert(stack.depth == 1);
  assert(scope_lookup(&stack, "p") == NULL); // out of scope now
  assert(scope_lookup(&stack, "g") != NULL); // global still visible

  // deliberately blow past the old fixed MAX_SCOPE_DEPTH (32) to prove
  // there's no ceiling anymore - push 100 nested scopes
  for (int i = 0; i < 100; i++) {
    scope_push(&stack);
  }
  assert(stack.depth == 101); // global + 100 pushed
  for (int i = 0; i < 100; i++) {
    scope_pop(&stack);
  }
  assert(stack.depth == 1);

  arena_free(&arena);

  printf("all scope stack smoke tests passed\n");
  return 0;
}