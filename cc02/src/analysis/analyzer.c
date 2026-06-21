#include "analyzer.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>


#define SCOPE_STACK_INITIAL_CAPACITY 8   // matches the starting capacity used by parser.c's scratch buffers


int analyzer_init(analyzer_t *a) {
  if (!arena_init(&a->arena, ANALYZER_SCOPE_ALLOC_SIZE))
    return 0;

  a->capacity = SCOPE_STACK_INITIAL_CAPACITY;
  a->scopes = arena_alloc(&a->arena, sizeof(symtab_t) * a->capacity);
  a->depth = 0;
  a->errors = 0;

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


int analyzer_insert_symbol(analyzer_t *a, char *key, symbol_t value) {
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

#define INSERT_SYMBOL(NODE, SYM)                                                           \
  do {                                                                                     \
    if (!analyzer_insert_symbol(a, SYM.name, SYM)) {                                       \
      error_t e = (error_t) {                                                              \
        .type = ERR_REDECLARATION,                                                         \
        .loc = NODE->loc,                                                                  \
        .name_error = { .name = SYM.name }                                                 \
      };                                                                                   \
                                                                                           \
      ++a->errors;                                                                         \
      print_error(&e);                                                                     \
    }                                                                                      \
  } while(0);


static const type_t TYPE_ERROR = { .kind = TYPE_VOID, .is_ptr = 0, .ptr_depth = 0 };
static const type_t TYPE_NULL  = { .kind = TYPE_VOID, .is_ptr = 1, .ptr_depth = 1 };

static int is_type_error(type_t t) {
  return t.kind == TYPE_VOID && !t.is_ptr && !t.ptr_depth;
}

static int type_width(type_kind_t kind) {
  switch (kind) {
    case TYPE_U8:  case TYPE_I8:  return 1;
    case TYPE_U16: case TYPE_I16: return 2;
    default: return 0;
  }
}

static int is_null_type(type_t t) {
  return t.kind == TYPE_VOID && t.is_ptr && t.ptr_depth == 1;
}

static int is_types_compatible(type_t expected, type_t actual) {
  // null/0 is compatible with any pointer or integer type
  if (is_null_type(actual)) return 1;
  if (is_null_type(expected) && actual.is_ptr) return 1;

  if (expected.is_ptr != actual.is_ptr) return 0;
  if (expected.ptr_depth != actual.ptr_depth) return 0;

  if (expected.kind == actual.kind) return 1;

  if (expected.kind == TYPE_STRUCT || actual.kind == TYPE_STRUCT) return 0;
  if (expected.kind == TYPE_VOID || actual.kind == TYPE_VOID) return 0;

  return type_width(actual.kind) <= type_width(expected.kind);
}


static type_t resolve_expr_type(analyzer_t *a, node_t *expr) {
  if (!expr) return TYPE_ERROR;
  switch (expr->kind) {
    case NODE_NUMBER: {
      long val = expr->number;
      if (val == 0)
        return TYPE_NULL;
      if (val >= 1 && val <= 255)
        return (type_t){ .kind = TYPE_U8 };
      if (val >= -128 && val <= -1)
        return (type_t){ .kind = TYPE_I8 };
      if (val >= 256 && val <= 65535)
        return (type_t){ .kind = TYPE_U16 };
      if (val >= -32768 && val <= -129)
        return (type_t){ .kind = TYPE_I16 };

      return TYPE_ERROR;
    }


    case NODE_STRING:
      return (type_t){ .kind = TYPE_U8, .is_ptr = 1, .ptr_depth = 1 };

    case NODE_IDENTIFIER: {
      symbol_t *sym = analyzer_lookup(a, expr->identifier);
      if (!sym) {
        error_t e = (error_t){
          .type = ERR_UNDECLARED_IDENTIFIER,
          .loc = expr->loc,
          .name_error = { .name = expr->identifier }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      if (sym->kind != SYMBOL_VARIABLE) {
        error_t e = (error_t){
          .type = ERR_NOT_ASSIGNABLE,
          .loc = expr->loc,
          .name_error = { .name = expr->identifier }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      return sym->variable.type;
    }

    case NODE_CALL: {
      symbol_t *sym = analyzer_lookup(a, expr->call.name);
      if (!sym) {
        error_t e = (error_t){
          .type = ERR_UNDECLARED_IDENTIFIER,
          .loc = expr->loc,
          .name_error = { .name = expr->call.name }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      if (sym->kind != SYMBOL_FUNCTION) {
        error_t e = (error_t){
          .type = ERR_NOT_A_FUNCTION,
          .loc = expr->loc,
          .name_error = { .name = expr->call.name }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      if (expr->call.args.count != sym->function.params.count) {
        error_t e = (error_t){
          .type = ERR_WRONG_ARG_COUNT,
          .loc = expr->loc,
          .arg_count = {
            .fn_name = expr->call.name,
            .expected_count = sym->function.params.count,
            .actual_count = expr->call.args.count
          }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      for (unsigned i = 0; i < expr->call.args.count; ++i) {
        type_t found = resolve_expr_type(a, expr->call.args.items[i]);
        if (!is_types_compatible(sym->function.params.items[i].type, found)) {
          error_t e = (error_t){
            .type = ERR_TYPE_MISMATCH,
            .loc = expr->loc,
            .type_mismatch = {
              .expected = sym->function.params.items[i].type,
              .actual = found,
              .context = "function call"
            }
          };
          ++a->errors;
          print_error(&e);
          return TYPE_ERROR;
        }
      }

      return sym->function.return_type;
    }

    case NODE_CAST:
      resolve_expr_type(a, expr->cast.operand);
      return expr->cast.cast_type;

    case NODE_DEREF: {
      type_t inner = resolve_expr_type(a, expr->deref_target);
      if (is_type_error(inner)) return TYPE_ERROR;

      if (!inner.is_ptr || inner.ptr_depth == 0) {
        error_t e = (error_t){
          .type = ERR_TYPE_MISMATCH,
          .loc = expr->loc,
          .type_mismatch = {
            .expected = { .kind = inner.kind, .is_ptr = 1, .ptr_depth = 1 },
            .actual = inner,
            .context = "dereference"
          }
        };
        print_error(&e);
        ++a->errors;
        return TYPE_ERROR;
      }

      type_t result = inner;
      result.ptr_depth--;
      if (result.ptr_depth == 0) result.is_ptr = 0;
      return result;
    }

    case NODE_UNARY: {
      type_t operand = resolve_expr_type(a, expr->unary.operand);
      if (is_type_error(operand)) return TYPE_ERROR;

      if (expr->unary.op == OP_ADDRESSOF) {
        type_t result = operand;
        result.is_ptr = 1;
        result.ptr_depth++;
        return result;
      }

      return operand;
    }

    case NODE_BINOP: {
      type_t left = resolve_expr_type(a, expr->binop.left);
      type_t right = resolve_expr_type(a, expr->binop.right);
      if (is_type_error(left) || is_type_error(right)) return TYPE_ERROR;

      if (!is_types_compatible(left, right) && !is_types_compatible(right, left)) {
        error_t e = (error_t) {
          .type = ERR_TYPE_MISMATCH,
          .loc = expr->loc,
          .type_mismatch = {
            .expected = left,
            .actual = right,
            .context = "binary operation"
          }
        };

        print_error(&e);
        ++a->errors;
        return TYPE_ERROR;
      }

      // result is the wider of the two
      if (type_width(right.kind) > type_width(left.kind))
        return right;
      return left;
    }

    case NODE_FIELD_ACCESS: {
      type_t base_type = resolve_expr_type(a, expr->field_access.base);
      if (is_type_error(base_type)) return TYPE_ERROR;

      if (base_type.kind != TYPE_STRUCT || base_type.is_ptr) {
        error_t e = (error_t){
          .type = ERR_TYPE_MISMATCH,
          .loc = expr->loc,
          .type_mismatch = {
            .expected = { .kind = TYPE_STRUCT },
            .actual = base_type,
            .context = "field access"
          }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      symbol_t *decl = analyzer_lookup(a, base_type.struct_name);
      if (!decl || decl->kind != SYMBOL_STRUCT) {
        error_t e = (error_t){
          .type = ERR_UNKNOWN_STRUCT,
          .loc = expr->loc,
          .name_error = { .name = base_type.struct_name }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      for (unsigned i = 0; i < decl->struct_decl.fields.count; i++) {
        if (strcmp(decl->struct_decl.fields.items[i].name, expr->field_access.field) == 0) {
          return decl->struct_decl.fields.items[i].type;
        }
      }

      error_t e = (error_t){
        .type = ERR_UNKNOWN_FIELD,
        .loc = expr->loc,
        .unknown_field = {
          .struct_name = base_type.struct_name,
          .field_name = expr->field_access.field
        }
      };
      ++a->errors;
      print_error(&e);
      return TYPE_ERROR;
    }

    case NODE_STRUCT_INIT: {
      symbol_t *decl = analyzer_lookup(a, expr->struct_init.struct_name);
      if (!decl || decl->kind != SYMBOL_STRUCT) {
        error_t e = (error_t){
          .type = ERR_UNKNOWN_STRUCT,
          .loc = expr->loc,
          .name_error = { .name = expr->struct_init.struct_name }
        };
        ++a->errors;
        print_error(&e);
        return TYPE_ERROR;
      }

      for (unsigned i = 0; i < expr->struct_init.inits.count; i++) {
        field_init_t *init = &expr->struct_init.inits.items[i];

        // find the matching field in the struct declaration
        int found = 0;
        for (unsigned j = 0; j < decl->struct_decl.fields.count; j++) {
          if (strcmp(decl->struct_decl.fields.items[j].name, init->field_name) == 0) {
            found = 1;
            type_t field_type = decl->struct_decl.fields.items[j].type;
            type_t init_type = resolve_expr_type(a, init->value);
            if (!is_type_error(init_type) && !is_types_compatible(field_type, init_type)) {
              error_t e = (error_t){
                .type = ERR_TYPE_MISMATCH,
                .loc = init->value->loc,
                .type_mismatch = {
                  .expected = field_type,
                  .actual = init_type,
                  .context = init->field_name
                }
              };
              ++a->errors;
              print_error(&e);
            }
            break;
          }
        }

        if (!found) {
          error_t e = (error_t){
            .type = ERR_UNKNOWN_FIELD,
            .loc = expr->loc,
            .unknown_field = {
              .struct_name = expr->struct_init.struct_name,
              .field_name = init->field_name
            }
          };
          ++a->errors;
          print_error(&e);
        }
      }

      return (type_t){ .kind = TYPE_STRUCT, .struct_name = expr->struct_init.struct_name };
    }

    default:
      return TYPE_ERROR;
  }
}


static void analyze_stmt(analyzer_t *a, node_t *node) {
  switch (node->kind) {
    case NODE_BLOCK: {
      analyzer_scope_push(a);
      for (unsigned i = 0; i < node->block.count; i++) {
        analyze_stmt(a, node->block.items[i]);
      }
    
        analyzer_scope_pop(a);
      break;
    }

    case NODE_STRUCT_DECL: {
      symbol_t sym = (symbol_t){
        .kind = SYMBOL_STRUCT,
        .name = node->struct_decl.name,
        .struct_decl = { .fields = node->struct_decl.fields }
      };
      INSERT_SYMBOL(node, sym);
      break;
    }

    case NODE_VAR_DECL: {
      if (node->var_decl.initialiser) {
        type_t init_type = resolve_expr_type(a, node->var_decl.initialiser);
        if (!is_type_error(init_type) && !is_types_compatible(node->var_decl.type, init_type)) {
          error_t e = (error_t){
            .type = ERR_TYPE_MISMATCH,
            .loc = node->loc,
            .type_mismatch = {
              .expected = node->var_decl.type,
              .actual = init_type,
              .context = node->var_decl.name
            }
          };
          ++a->errors;
          print_error(&e);
        }
      }

      symbol_t sym = (symbol_t){
        .kind = SYMBOL_VARIABLE,
        .name = node->var_decl.name,
        .variable = {
          .type = node->var_decl.type,
          .is_register = 0,
          .addr = 0
        }
      };

      INSERT_SYMBOL(node, sym);
      break;
    }

    case NODE_ASSIGN: {
      type_t target_type = resolve_expr_type(a, node->assign.target);
      type_t value_type = resolve_expr_type(a, node->assign.value);
      if (!is_type_error(target_type) && !is_type_error(value_type)
          && !is_types_compatible(target_type, value_type)) {
        error_t e = (error_t){
          .type = ERR_TYPE_MISMATCH,
          .loc = node->loc,
          .type_mismatch = {
            .expected = target_type,
            .actual = value_type,
            .context = "assignment"
          }
        };
        ++a->errors;
        print_error(&e);
      }
      break;
    }

    case NODE_RETURN: {
      if (node->return_val) {
        type_t ret_type = resolve_expr_type(a, node->return_val);
        if (!is_type_error(ret_type) && !is_types_compatible(a->current_return_type, ret_type)) {
          error_t e = (error_t){
            .type = ERR_TYPE_MISMATCH,
            .loc = node->loc,
            .type_mismatch = {
              .expected = a->current_return_type,
              .actual = ret_type,
              .context = "return"
            }
          };
          ++a->errors;
          print_error(&e);
        }
      }
      break;
    }

    case NODE_IF: {
      resolve_expr_type(a, node->if_stmt.cond);
      for (unsigned i = 0; i < node->if_stmt.blocks.count; i++) {
        analyze_stmt(a, node->if_stmt.blocks.items[i]);
      }
      break;
    }

    case NODE_WHILE:
      resolve_expr_type(a, node->while_stmt.cond);
      if (node->while_stmt.body)
        analyze_stmt(a, node->while_stmt.body);
      break;

    case NODE_FOR:
      analyzer_scope_push(a);
      if (node->for_stmt.initialiser)
        analyze_stmt(a, node->for_stmt.initialiser);
      if (node->for_stmt.cond)
        resolve_expr_type(a, node->for_stmt.cond);
      if (node->for_stmt.incrementer)
        analyze_stmt(a, node->for_stmt.incrementer);
      if (node->for_stmt.body)
        analyze_stmt(a, node->for_stmt.body);
      analyzer_scope_pop(a);
      break;

    default:
      resolve_expr_type(a, node);
      break;
  }
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

    INSERT_SYMBOL(decl, sym);
  }
}


static void pass2_entry(analyzer_t *a, ast_t program) {
  for (unsigned i = 0; i < program->program.count; ++i) {
    node_t *node = program->program.items[i];
    if (node->kind == NODE_FUNCTION) {
      a->current_return_type = node->function.return_type;
      analyzer_scope_push(a);

      for (unsigned j = 0; j < node->function.params.count; ++j) {
        param_t p = node->function.params.items[j];

        symbol_t sym = (symbol_t) {
          .kind = SYMBOL_VARIABLE,
          .name = p.name,
          .variable = {
            .type = p.type,
            .is_register = 0,
            .addr = 0
          }
        };

        if (!analyzer_insert_symbol(a, sym.name, sym)) {
          error_t e = (error_t){
            .type = ERR_REDECLARATION,
            .loc = node->loc,
            .name_error = { .name = sym.name }
          };

          ++a->errors;
          print_error(&e);
        }
      }

      // analyze block
      for (unsigned k = 0; k < node->function.body->block.count; k++)
        analyze_stmt(a, node->function.body->block.items[k]);

      analyzer_scope_pop(a);
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
      .loc = { .file_path = ast->loc.file_path, .column = 0, .line = 1, .length = 0 },
      .name_error = { .name = "main" }
    };

    ++a->errors;
    print_error(&e);
  }

  pass2_entry(a, ast);

  return analyzer_global_scope(a);
}