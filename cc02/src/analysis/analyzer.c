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


/*
 * Inserts SYM into the current scope via analyzer_insert_symbol().
 * Emits ERR_REDECLARATION if the name already exists in this scope.
 * Assumes `a` (analyzer_t *) is in scope.
 */
#define INSERT_SYMBOL(NODE, SYM)                                                           \
  do {                                                                                     \
    if (!analyzer_insert_symbol(a, SYM.name, SYM)) {                                       \
      EMIT_NAME_ERROR(ERR_REDECLARATION, NODE->loc, SYM.name);                             \
    }                                                                                      \
  } while(0)


/*
 * Emits a name-based error (undeclared identifier, not-a-function,
 * unknown struct, redeclaration, not-assignable). These all share the
 * same error_t shape: a type, a location, and a single name string.
 * Assumes `a` (analyzer_t *) is in scope.
 */
#define EMIT_NAME_ERROR(ERR_TYPE, LOC, NAME)                                               \
  do {                                                                                     \
    error_t _e = (error_t){                                                                \
      .type = ERR_TYPE, .loc = LOC,                                                        \
      .name_error = { .name = NAME }                                                       \
    };                                                                                     \
    ++a->errors;                                                                           \
    print_error(&_e);                                                                      \
  } while(0)


/*
 * Emits a type mismatch error with expected/actual types and a context
 * string (e.g. "assignment", "return", a variable name). Covers
 * narrowing, pointer/int mismatches, and incompatible operands.
 * Assumes `a` (analyzer_t *) is in scope.
 */
#define EMIT_TYPE_ERROR(LOC, EXPECTED, ACTUAL, CTX)                                        \
  do {                                                                                     \
    error_t _e = (error_t){                                                                \
      .type = ERR_TYPE_MISMATCH, .loc = LOC,                                               \
      .type_mismatch = { .expected = EXPECTED, .actual = ACTUAL, .context = CTX }          \
    };                                                                                     \
    ++a->errors;                                                                           \
    print_error(&_e);                                                                      \
  } while(0)


/*
 * Emits an unknown-field error for struct field access or struct
 * initializer when a named field doesn't exist in the declaration.
 * Assumes `a` (analyzer_t *) is in scope.
 */
#define EMIT_FIELD_ERROR(LOC, SNAME, FNAME)                                                \
  do {                                                                                     \
    error_t _e = (error_t){                                                                \
      .type = ERR_UNKNOWN_FIELD, .loc = LOC,                                               \
      .unknown_field = { .struct_name = SNAME, .field_name = FNAME }                       \
    };                                                                                     \
    ++a->errors;                                                                           \
    print_error(&_e);                                                                      \
  } while(0)


/* Emits an error that carries only a location (no payload). */
#define EMIT_PLAIN_ERROR(ERR_TYPE, LOC)                                                    \
  do {                                                                                     \
    error_t _e = (error_t){ .type = ERR_TYPE, .loc = LOC };                                \
    ++a->errors;                                                                           \
    print_error(&_e);                                                                      \
  } while(0)


/* Emits a non-lvalue error with a ready-to-print message (assignment, &, ++/--). */
#define EMIT_LVALUE_ERROR(LOC, MSG)                                                        \
  do {                                                                                     \
    error_t _e = (error_t){                                                                \
      .type = ERR_NOT_LVALUE, .loc = LOC, .lvalue = { .message = MSG }                     \
    };                                                                                     \
    ++a->errors;                                                                           \
    print_error(&_e);                                                                      \
  } while(0)


/* Emits a field-access-on-non-struct error (FIELD accessed on ACTUAL type). */
#define EMIT_NOT_A_STRUCT(LOC, ACTUAL, FIELD)                                              \
  do {                                                                                     \
    error_t _e = (error_t){                                                                \
      .type = ERR_NOT_A_STRUCT, .loc = LOC,                                                \
      .type_mismatch = { .actual = ACTUAL, .context = FIELD }                              \
    };                                                                                     \
    ++a->errors;                                                                           \
    print_error(&_e);                                                                      \
  } while(0)


static const type_t TYPE_ERROR = { .kind = TYPE_INVALID, .is_ptr = 0, .ptr_depth = 0 };
static const type_t TYPE_NULL  = { .kind = TYPE_VOID, .is_ptr = 1, .ptr_depth = 1 };


static int is_type_error(type_t t) {
  return t.kind == TYPE_INVALID;
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


// Smallest type that holds integer literal `val`. 0 is treated as null (so it
// stays compatible with any pointer or integer). Sets *out_of_range if no
// supported type fits - the lexer produces only non-negative literals, so the
// negative ranges are reached via a negated literal (see NODE_UNARY).
static type_t type_for_int_literal(long val, int *out_of_range) {
  *out_of_range = 0;
  if (val == 0)                     return TYPE_NULL;
  if (val >= 1 && val <= 255)       return (type_t){ .kind = TYPE_U8 };
  if (val >= -128 && val <= -1)     return (type_t){ .kind = TYPE_I8 };
  if (val >= 256 && val <= 65535)   return (type_t){ .kind = TYPE_U16 };
  if (val >= -32768 && val <= -129) return (type_t){ .kind = TYPE_I16 };

  *out_of_range = 1;
  return (type_t){ .kind = TYPE_INVALID };
}


// An lvalue (assignable / addressable) is a named variable, a struct field, or
// a pointer dereference - never a literal, call result, or computed value.
static int is_lvalue(const node_t *n) {
  return n && (n->kind == NODE_IDENTIFIER
            || n->kind == NODE_FIELD_ACCESS
            || n->kind == NODE_DEREF);
}


static int is_types_compatible(type_t expected, type_t actual) {
  // null/0 is compatible with any pointer or integer type
  if (is_null_type(actual)) return 1;
  if (is_null_type(expected) && actual.is_ptr) return 1;

  if (expected.is_ptr != actual.is_ptr) return 0;
  if (expected.ptr_depth != actual.ptr_depth) return 0;

  // two struct types match only if they name the same struct - sharing the
  // TYPE_STRUCT kind isn't enough, or Point and Line would be interchangeable
  if (expected.kind == TYPE_STRUCT && actual.kind == TYPE_STRUCT) {
    return expected.struct_name && actual.struct_name
        && strcmp(expected.struct_name, actual.struct_name) == 0;
  }

  if (expected.kind == actual.kind) return 1;

  if (expected.kind == TYPE_STRUCT || actual.kind == TYPE_STRUCT) return 0;
  if (expected.kind == TYPE_VOID || actual.kind == TYPE_VOID) return 0;

  return type_width(actual.kind) <= type_width(expected.kind);
}


// Validates a type that appears in a declaration. A struct type must name a
// struct that exists (run after pass1, so forward references resolve). A
// non-pointer void is rejected for variables/params/fields/globals (void* is
// fine - it's a null pointer); `allow_void` is set only for function return
// types, where bare void is legal. `loc`/`name` locate the diagnostic.
// Returns 1 if the type is a legal declaration type, 0 (after emitting a
// diagnostic) otherwise. Callers that register a symbol should poison its type
// to TYPE_INVALID on a 0 return, so later uses of the name don't re-report the
// same bad type (one error per bad declaration, not one per use).
static int validate_decl_type(analyzer_t *a, type_t type, token_location_t loc,
                              char *name, int allow_void) {
  if (!allow_void && type.kind == TYPE_VOID && !type.is_ptr) {
    EMIT_NAME_ERROR(ERR_VOID_VARIABLE, loc, name);
    return 0;
  }

  if (type.kind == TYPE_STRUCT) {
    symbol_t *s = analyzer_lookup(a, type.struct_name);
    if (!s || s->kind != SYMBOL_STRUCT) {
      EMIT_NAME_ERROR(ERR_UNKNOWN_STRUCT, loc, type.struct_name);
      return 0;
    }
  }

  return 1;
}


static type_t resolve_expr_type(analyzer_t *a, node_t *expr) {
  if (!expr) return TYPE_ERROR;
  switch (expr->kind) {
    case NODE_NUMBER: {
      int out_of_range = 0;
      type_t t = type_for_int_literal(expr->number, &out_of_range);
      if (out_of_range) {
        EMIT_PLAIN_ERROR(ERR_LITERAL_OUT_OF_RANGE, expr->loc);
        return TYPE_ERROR;
      }
      return t;
    }


    case NODE_STRING:
      return (type_t){ .kind = TYPE_U8, .is_ptr = 1, .ptr_depth = 1 };

    case NODE_IDENTIFIER: {
      symbol_t *sym = analyzer_lookup(a, expr->identifier.name);
      if (!sym) {
        EMIT_NAME_ERROR(ERR_UNDECLARED_IDENTIFIER, expr->loc, expr->identifier.name);
        return TYPE_ERROR;
      }

      if (sym->kind != SYMBOL_VARIABLE) {
        EMIT_NAME_ERROR(ERR_NOT_ASSIGNABLE, expr->loc, expr->identifier.name);
        return TYPE_ERROR;
      }

      expr->identifier.resolved_type = sym->variable.type;
      return sym->variable.type;
    }

    case NODE_CALL: {
      symbol_t *sym = analyzer_lookup(a, expr->call.name);
      if (!sym) {
        EMIT_NAME_ERROR(ERR_UNDECLARED_IDENTIFIER, expr->loc, expr->call.name);
        return TYPE_ERROR;
      }

      if (sym->kind != SYMBOL_FUNCTION) {
        EMIT_NAME_ERROR(ERR_NOT_A_FUNCTION, expr->loc, expr->call.name);
        return TYPE_ERROR;
      }

      if (expr->call.args.count != sym->function.params.count) {
        error_t _e = (error_t){
          .type = ERR_WRONG_ARG_COUNT, .loc = expr->loc,
          .arg_count = {
            .fn_name = expr->call.name,
            .expected_count = sym->function.params.count,
            .actual_count = expr->call.args.count
          }
        };
        ++a->errors;
        print_error(&_e);
        return TYPE_ERROR;
      }

      for (unsigned i = 0; i < expr->call.args.count; ++i) {
        type_t found = resolve_expr_type(a, expr->call.args.items[i]);
        if (is_type_error(found)) continue;   // arg already reported its own error
        if (!is_types_compatible(sym->function.params.items[i].type, found)) {
          EMIT_TYPE_ERROR(expr->loc, sym->function.params.items[i].type, found, "function call");
          return TYPE_ERROR;
        }
      }

      expr->call.resolved_return_type = sym->function.return_type;
      return sym->function.return_type;
    }

    case NODE_CAST:
      resolve_expr_type(a, expr->cast.operand);
      return expr->cast.cast_type;

    case NODE_DEREF: {
      type_t inner = resolve_expr_type(a, expr->deref_target);
      if (is_type_error(inner)) return TYPE_ERROR;

      if (!inner.is_ptr || inner.ptr_depth == 0) {
        EMIT_TYPE_ERROR(expr->loc,
          ((type_t){ .kind = inner.kind, .is_ptr = 1, .ptr_depth = 1 }),
          inner, "dereference");
        return TYPE_ERROR;
      }

      type_t result = inner;
      result.ptr_depth--;
      if (result.ptr_depth == 0) result.is_ptr = 0;
      return result;
    }

    case NODE_UNARY: {
      // A negated integer literal (-5, -300) is typed from its negative value,
      // so -5 is i8 not u8. The lexer only ever emits non-negative literals, so
      // unary minus on a literal is where a negative literal's type is decided.
      if (expr->unary.op == OP_NEGATE && expr->unary.operand
          && expr->unary.operand->kind == NODE_NUMBER) {
        int out_of_range = 0;
        type_t t = type_for_int_literal(-expr->unary.operand->number, &out_of_range);
        if (out_of_range) {
          EMIT_PLAIN_ERROR(ERR_LITERAL_OUT_OF_RANGE, expr->loc);
          return TYPE_ERROR;
        }
        return t;
      }

      type_t operand = resolve_expr_type(a, expr->unary.operand);
      if (is_type_error(operand)) return TYPE_ERROR;

      if (expr->unary.op == OP_ADDRESSOF) {
        if (!is_lvalue(expr->unary.operand)) {
          EMIT_LVALUE_ERROR(expr->loc, "cannot take the address of a non-lvalue");
          return TYPE_ERROR;
        }
        type_t result = operand;
        result.is_ptr = 1;
        result.ptr_depth++;
        return result;
      }

      if (expr->unary.op == OP_INCREMENT || expr->unary.op == OP_DECREMENT) {
        if (!is_lvalue(expr->unary.operand)) {
          EMIT_LVALUE_ERROR(expr->loc,
            expr->unary.op == OP_INCREMENT ? "cannot increment a non-lvalue"
                                           : "cannot decrement a non-lvalue");
          return TYPE_ERROR;
        }
      }

      return operand;
    }

    case NODE_BINOP: {
      type_t left = resolve_expr_type(a, expr->binop.left);
      type_t right = resolve_expr_type(a, expr->binop.right);
      if (is_type_error(left) || is_type_error(right)) return TYPE_ERROR;

      if (!is_types_compatible(left, right) && !is_types_compatible(right, left)) {
        EMIT_TYPE_ERROR(expr->loc, left, right, "binary operation");
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

      // auto-deref a single-level struct pointer: there is no -> operator, so
      // `c.field` on a Struct* peels one pointer level. Deeper pointers
      // (Struct**) need an explicit * and are not auto-deref'd here.
      if (base_type.kind == TYPE_STRUCT && base_type.is_ptr && base_type.ptr_depth == 1) {
        base_type.is_ptr = 0;
        base_type.ptr_depth = 0;
      }

      if (base_type.kind != TYPE_STRUCT || base_type.is_ptr) {
        EMIT_NOT_A_STRUCT(expr->loc, base_type, expr->field_access.field);
        return TYPE_ERROR;
      }

      symbol_t *decl = analyzer_lookup(a, base_type.struct_name);
      if (!decl || decl->kind != SYMBOL_STRUCT) {
        EMIT_NAME_ERROR(ERR_UNKNOWN_STRUCT, expr->loc, base_type.struct_name);
        return TYPE_ERROR;
      }

      for (unsigned i = 0; i < decl->struct_decl.fields.count; i++) {
        if (strcmp(decl->struct_decl.fields.items[i].name, expr->field_access.field) == 0) {
          expr->field_access.resolved_type = decl->struct_decl.fields.items[i].type;
          return decl->struct_decl.fields.items[i].type;
        }
      }

      EMIT_FIELD_ERROR(expr->loc, base_type.struct_name, expr->field_access.field);
      return TYPE_ERROR;
    }

    case NODE_STRUCT_INIT: {
      symbol_t *decl = analyzer_lookup(a, expr->struct_init.struct_name);
      if (!decl || decl->kind != SYMBOL_STRUCT) {
        EMIT_NAME_ERROR(ERR_UNKNOWN_STRUCT, expr->loc, expr->struct_init.struct_name);
        return TYPE_ERROR;
      }

      for (unsigned i = 0; i < expr->struct_init.inits.count; i++) {
        field_init_t *init = &expr->struct_init.inits.items[i];

        int found = 0;
        for (unsigned j = 0; j < decl->struct_decl.fields.count; j++) {
          if (strcmp(decl->struct_decl.fields.items[j].name, init->field_name) == 0) {
            found = 1;
            type_t field_type = decl->struct_decl.fields.items[j].type;
            type_t init_type = resolve_expr_type(a, init->value);
            if (!is_type_error(init_type) && !is_types_compatible(field_type, init_type)) {
              EMIT_TYPE_ERROR(init->value->loc, field_type, init_type, init->field_name);
            }
            break;
          }
        }

        if (!found) {
          EMIT_FIELD_ERROR(expr->loc, expr->struct_init.struct_name, init->field_name);
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
      type_t decl_type = node->var_decl.type;
      if (!validate_decl_type(a, decl_type, node->loc, node->var_decl.name, 0)) {
        decl_type = TYPE_ERROR;  // poison: later uses of this name won't re-report
      }

      if (node->var_decl.initialiser) {
        type_t init_type = resolve_expr_type(a, node->var_decl.initialiser);
        if (!is_type_error(decl_type) && !is_type_error(init_type)
            && !is_types_compatible(decl_type, init_type)) {
          EMIT_TYPE_ERROR(node->loc, decl_type, init_type, node->var_decl.name);
        }
      }

      symbol_t sym = (symbol_t){
        .kind = SYMBOL_VARIABLE,
        .name = node->var_decl.name,
        .variable = {
          .type = decl_type,
          .is_register = 0,
          .addr = 0
        }
      };

      INSERT_SYMBOL(node, sym);
      break;
    }

    case NODE_ASSIGN: {
      int assignable = is_lvalue(node->assign.target);
      if (!assignable) {
        EMIT_LVALUE_ERROR(node->loc, "left-hand side of assignment is not assignable");
      }
      type_t target_type = resolve_expr_type(a, node->assign.target);
      type_t value_type = resolve_expr_type(a, node->assign.value);
      if (assignable && !is_type_error(target_type) && !is_type_error(value_type)
          && !is_types_compatible(target_type, value_type)) {
        EMIT_TYPE_ERROR(node->loc, target_type, value_type, "assignment");
      }
      break;
    }

    case NODE_RETURN: {
      if (node->return_val) {
        type_t ret_type = resolve_expr_type(a, node->return_val);
        if (!is_type_error(ret_type) && !is_types_compatible(a->current_return_type, ret_type)) {
          EMIT_TYPE_ERROR(node->loc, a->current_return_type, ret_type, "return");
        }
      } else {
        // a bare `return;` is only valid when the enclosing function returns void
        type_t rt = a->current_return_type;
        if (!(rt.kind == TYPE_VOID && !rt.is_ptr)) {
          EMIT_TYPE_ERROR(node->loc, rt, ((type_t){ .kind = TYPE_VOID }), "return");
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


// After pass1 has registered every top-level symbol, validate the types that
// appear in top-level declarations: globals, registers, struct fields, and
// function return types. Run as its own sweep (rather than during pass1) so a
// forward reference to a struct declared further down still resolves.
static void validate_toplevel_types(analyzer_t *a, ast_t program) {
  for (unsigned i = 0; i < program->program.count; ++i) {
    node_t *decl = program->program.items[i];
    switch (decl->kind) {
      case NODE_GLOBAL_VAR: {
        type_t gtype = decl->global_var.type;
        if (!validate_decl_type(a, gtype, decl->loc, decl->global_var.name, 0)) {
          gtype = TYPE_ERROR;
          symbol_t *s = analyzer_lookup(a, decl->global_var.name);
          if (s && s->kind == SYMBOL_VARIABLE) s->variable.type = TYPE_ERROR;  // poison
        }
        // type-check the initializer too - resolved in the global scope, where
        // all globals and functions are visible (pass1 has registered them)
        if (decl->global_var.initialiser) {
          type_t it = resolve_expr_type(a, decl->global_var.initialiser);
          if (!is_type_error(gtype) && !is_type_error(it) && !is_types_compatible(gtype, it)) {
            EMIT_TYPE_ERROR(decl->loc, gtype, it, decl->global_var.name);
          }
        }
        break;
      }
      case NODE_REG_DECL:
        if (!validate_decl_type(a, decl->reg_decl.type, decl->loc, decl->reg_decl.name, 0)) {
          symbol_t *s = analyzer_lookup(a, decl->reg_decl.name);
          if (s && s->kind == SYMBOL_VARIABLE) s->variable.type = TYPE_ERROR;  // poison
        }
        break;
      case NODE_STRUCT_DECL:
        for (unsigned f = 0; f < decl->struct_decl.fields.count; ++f) {
          field_t field = decl->struct_decl.fields.items[f];
          validate_decl_type(a, field.type, decl->loc, field.name, 0);
        }
        break;
      case NODE_FUNCTION:
        // return type may legitimately be void; params are validated in pass2
        validate_decl_type(a, decl->function.return_type, decl->loc, decl->function.name, 1);
        break;
      default:
        break;
    }
  }
}


// Conservative "could this statement hand back a return value" test for the
// shallow missing-return check. Only `return` is a definite yes; control-flow
// statements are assumed to maybe-return so we don't false-positive on a
// function ending in an if/else that returns. NULL (an empty body) is a no.
// This is intentionally shallow: it won't catch a one-armed `if` that falls
// through (see CHANGELOG).
static int stmt_may_return(const node_t *n) {
  if (!n) return 0;
  switch (n->kind) {
    case NODE_RETURN:
    case NODE_IF:
    case NODE_WHILE:
    case NODE_FOR:
    case NODE_BLOCK:
      return 1;
    default:
      return 0;
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

        type_t ptype = p.type;
        if (!validate_decl_type(a, ptype, node->loc, p.name, 0)) {
          ptype = TYPE_ERROR;  // poison so uses of the param don't re-report
        }

        symbol_t sym = (symbol_t) {
          .kind = SYMBOL_VARIABLE,
          .name = p.name,
          .variable = {
            .type = ptype,
            .is_register = 0,
            .addr = 0
          }
        };

        if (!analyzer_insert_symbol(a, sym.name, sym)) {
          EMIT_NAME_ERROR(ERR_REDECLARATION, node->loc, sym.name);
        }
      }

      // analyze block
      for (unsigned k = 0; k < node->function.body->block.count; k++)
        analyze_stmt(a, node->function.body->block.items[k]);

      // shallow missing-return check: a non-void function whose body can't be
      // seen to end in a return is flagged (see stmt_may_return).
      type_t rt = node->function.return_type;
      if (!(rt.kind == TYPE_VOID && !rt.is_ptr)) {
        node_list_t body = node->function.body->block;
        node_t *last = body.count ? body.items[body.count - 1] : NULL;
        if (!stmt_may_return(last)) {
          EMIT_NAME_ERROR(ERR_MISSING_RETURN, node->loc, node->function.name);
        }
      }

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
  validate_toplevel_types(a, ast);

  symbol_t *main_sym = analyzer_lookup(a, "main");
  if (!main_sym || main_sym->kind != SYMBOL_FUNCTION) {
    error_t _e = (error_t){
      .type = ERR_MISSING_MAIN,
      .loc = (token_location_t){ .file_path = ast->loc.file_path }
    };
    ++a->errors;
    print_error(&_e);
  }

  pass2_entry(a, ast);

  return analyzer_global_scope(a);
}