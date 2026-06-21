#include "analyzer.h"

#include <stdio.h>
#include <assert.h>


#define SCOPE_STACK_INITIAL_CAPACITY 8   // matches the starting capacity used by parser.c's scratch buffers


int analyzer_init(analyzer_t *a) {
  if (!arena_init(&a->arena, ANALYZER_SCOPE_ALLOC_SIZE))
    return 0;

  a->capacity = SCOPE_STACK_INITIAL_CAPACITY;
  a->scopes = arena_alloc(&a->arena, sizeof(symtab_t) * a->capacity);
  a->depth = 0;
  a->has_errored = 0;

  analyzer_scope_push(a);

  return 1;
}


void analyzer_free(analyzer_t *a) {
  if (a) {
    arena_free(&a->arena);
  }
}


void analyzer_scope_push(analyzer_t *a) {
  if (a->depth == a->capacity) {
    unsigned new_capacity = a->capacity * 2;
    symtab_t *grown = arena_alloc(&a->arena, sizeof(symtab_t) * new_capacity);

    for (unsigned i = 0; i < a->depth; i++) {
      grown[i] = a->scopes[i];
    }

    a->scopes = grown;
    a->capacity = new_capacity;
  }

  symtab_init(&a->scopes[a->depth]);
  a->depth++;
}


void analyzer_scope_pop(analyzer_t *a) {
  assert(a->depth > 1 && "analyzer_scope_pop: attempted to pop the global scope");
  a->depth--;
}


int analyzer_insert_local(analyzer_t *a, char *key, symbol_t value) {
  symtab_t *current = &a->scopes[a->depth - 1];
  return symtab_insert(current, key, value, &a->arena);
}


symbol_t *analyzer_lookup(analyzer_t *a, const char *key) {
  for (unsigned i = a->depth; i > 0; i--) {
    symbol_t *found = symtab_lookup(&a->scopes[i - 1], key);
    if (found) {
      return found;
    }
  }
  return NULL;
}


/*
 * Reduces the repetitive case-decl pattern to a
 * single line per node kind. Expands to a switch case that builds a
 * symbol_t from the corresponding node_t union member.
 *
 *   NODE_TYPE          - node_kind_t enum value (e.g. NODE_FUNCTION)
 *   NODE_STRUCT_NAME   - node_t union member name (e.g. function)
 *   SYMBOL_TYPE        - symbol_kind_t enum value (e.g. SYMBOL_FUNCTION)
 *   SYMBOL_STRUCT_NAME - symbol_t union member name (e.g. function)
 *   ...                - designated initialiser fields for the symbol's
 *                        union member (variadic to allow commas)
 */
#define REGISTER_SYMBOL(NODE_TYPE, NODE_STRUCT_NAME, SYMBOL_TYPE, SYMBOL_STRUCT_NAME, ...) \
  case NODE_TYPE: {                                                                        \
    sym = (symbol_t){                                                                      \
      .kind = SYMBOL_TYPE,                                                                 \
      .name = decl->NODE_STRUCT_NAME.name,                                                 \
      .SYMBOL_STRUCT_NAME = { __VA_ARGS__ }                                                \
    };                                                                                     \
    break;                                                                                 \
  }


static void pass1_register_globals(analyzer_t *a, ast_t program) {
  for (unsigned i = 0; i < program->program.count; ++i) {
    node_t *decl = program->program.items[i];
    symbol_t sym;

    switch (decl->kind) {
      REGISTER_SYMBOL(NODE_REG_DECL, reg_decl, SYMBOL_VARIABLE, variable, 
        .type = decl->reg_decl.type,
        .is_register = 1,
        .addr = decl->reg_decl.addr,
      )
      
      REGISTER_SYMBOL(NODE_GLOBAL_VAR, global_var, SYMBOL_VARIABLE, variable,
        .type = decl->global_var.type,
        .is_register = 0,
        .addr = 0,
      )

      REGISTER_SYMBOL(NODE_STRUCT_DECL, struct_decl, SYMBOL_STRUCT, struct_decl,
        .fields = decl->struct_decl.fields
      )

      REGISTER_SYMBOL(NODE_FUNCTION, function, SYMBOL_FUNCTION, function, 
        .params = decl->function.params,
        .return_type = decl->function.return_type
      )

      default: assert(0 && "Unreachable!");
    }

    if (!analyzer_insert_local(a, sym.name, sym)) {
      error_t e = (error_t){
        .type = ERR_REDECLARATION,
        .loc = decl->loc,
        .name_error = { .name = sym.name }
      };

      a->has_errored = 1;
      print_error(&e);
    }
  }
}


symtab_t *analyzer_global_scope(analyzer_t *a) {
  return &a->scopes[0];
}


symtab_t *analyze(analyzer_t *a, ast_t ast) {
  if (!ast || ast->kind != NODE_PROGRAM || ast->program.count == 0) {
    fprintf(stderr, "Error, empty translation unit\n");
    return NULL;
  }

  pass1_register_globals(a, ast);

  // if no main function found, error
  symbol_t *main;
  if (!(main = analyzer_lookup(a, "main")) || main->kind != SYMBOL_FUNCTION) {
    error_t e = (error_t) {
      .type = ERR_UNDECLARED_IDENTIFIER,
      .loc = ast->loc,
      .name_error = { .name = "main" }
    };

    a->has_errored = 1;
    print_error(&e);
  }

  return analyzer_global_scope(a);
}