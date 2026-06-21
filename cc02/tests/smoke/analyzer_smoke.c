#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "analyzer.h"

int main(void) {
  analyzer_t a;
  analyzer_init(&a);
  assert(a.depth == 1); // just global

  // insert into global
  symbol_t g = { .name = "g", .kind = SYMBOL_VARIABLE };
  g.variable.type.kind = TYPE_U8;
  assert(analyzer_insert_symbol(&a, "g", g) == 1);

  // push a function scope, insert a param
  analyzer_scope_push(&a);
  symbol_t p = { .name = "p", .kind = SYMBOL_VARIABLE };
  p.variable.type.kind = TYPE_U16;
  assert(analyzer_insert_symbol(&a, "p", p) == 1);

  // lookup from inner scope sees both inner and outer
  assert(analyzer_lookup(&a, "p") != NULL);
  assert(analyzer_lookup(&a, "g") != NULL);
  assert(analyzer_lookup(&a, "g")->variable.type.kind == TYPE_U8);

  // shadowing: same name in a nested scope is allowed, and inner wins
  analyzer_scope_push(&a);
  symbol_t shadow = { .name = "g", .kind = SYMBOL_VARIABLE };
  shadow.variable.type.kind = TYPE_I16;
  assert(analyzer_insert_symbol(&a, "g", shadow) == 1); // allowed - different scope than outer "g"
  assert(analyzer_lookup(&a, "g")->variable.type.kind == TYPE_I16); // inner wins
  analyzer_scope_pop(&a);
  assert(analyzer_lookup(&a, "g")->variable.type.kind == TYPE_U8); // back to outer

  // redeclaration within the SAME scope is rejected
  symbol_t dup = { .name = "p", .kind = SYMBOL_VARIABLE };
  dup.variable.type.kind = TYPE_U8;
  assert(analyzer_insert_symbol(&a, "p", dup) == 0);

  analyzer_scope_pop(&a); // back to just global
  assert(a.depth == 1);
  assert(analyzer_lookup(&a, "p") == NULL); // out of scope now
  assert(analyzer_lookup(&a, "g") != NULL); // global still visible

  // deliberately blow past the old fixed MAX_SCOPE_DEPTH (32) to prove
  // there's no ceiling anymore - push 100 nested scopes
  for (int i = 0; i < 100; i++) {
    analyzer_scope_push(&a);
  }
  assert(a.depth == 101); // global + 100 pushed
  for (int i = 0; i < 100; i++) {
    analyzer_scope_pop(&a);
  }
  assert(a.depth == 1);

  analyzer_free(&a);

  printf("all analyzer smoke tests passed\n");
  return 0;
}