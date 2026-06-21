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


static void pass1_register_globals(analyzer_t *a, ast_t program) {
  for (unsigned i = 0; i < program->program.count; ++i) {
    node_t *decl = program->program.items[i];
    symbol_t sym;

    switch (decl->kind) {
      case NODE_REG_DECL:  {
        sym = (symbol_t){
          .kind = SYMBOL_VARIABLE,
          .name = decl->reg_decl.name,
          .variable = {
            .type = decl->reg_decl.type,
            .is_register = 1,
            .addr = decl->reg_decl.addr,
          }
        };

        break;
      }

      case NODE_GLOBAL_VAR: {
        sym = (symbol_t){
          .kind = SYMBOL_VARIABLE,
          .name = decl->global_var.name,
          .variable = {
            .type = decl->global_var.type,
            .is_register = 0,
            .addr = 0,
          }
        };

        break;
      }

      case NODE_STRUCT_DECL: {
        sym = (symbol_t){
          .kind = SYMBOL_STRUCT,
          .name = decl->struct_decl.name,
          .struct_decl = {
            .fields = decl->struct_decl.fields
          }
        };

        break;
      }

      case NODE_FUNCTION: {
        sym = (symbol_t){
          .kind = SYMBOL_FUNCTION,
          .name = decl->function.name,
          .function = {
            .params = decl->function.params,
            .return_type = decl->function.return_type
          }
        };

        break;
      }

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

  return analyzer_global_scope(a);
}