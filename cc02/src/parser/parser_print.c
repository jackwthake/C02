#include "parser.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "colors.h"

#define AST_PRINT_MAX_DEPTH 128

static void print_ast_(node_t *node, int is_last, const char *prefix);


static const char *type_name(type_kind_t kind) {
  switch (kind) {
    case TYPE_U8:  return "u8";
    case TYPE_I8:  return "i8";
    case TYPE_U16: return "u16";
    case TYPE_I16: return "i16";
    case TYPE_VOID:return "void";
    default:       return "<unknown>";
  }
}


static const char *op_name(op_t op) {
  switch (op) {
    case OP_INCREMENT:     return "++";
    case OP_DECREMENT:     return "--";
    case OP_PLUS:          return "+";
    case OP_MINUS:         return "-";
    case OP_MODULUS:       return "%";
    case OP_MULTIPLY:      return "*";
    case OP_DIVIDE:        return "/";
    case OP_LT:            return "<";
    case OP_GT:            return ">";
    case OP_LTE:           return "<=";
    case OP_GTE:           return ">=";
    case OP_EQUALSEQUALS:  return "==";
    case OP_BANGEQUALS:    return "!=";
    case OP_BANG:          return "!";
    case OP_NEGATE:        return "-";
    case OP_ADDRESSOF:     return "&";
    case OP_BAND:          return "&";
    case OP_BOR:           return "|";
    case OP_BXOR:          return "^";
    case OP_BNOT:          return "~";
    case OP_LEFT_SHIFT:    return "<<";
    case OP_RIGHT_SHIFT:   return ">>";
    case OP_AND:           return "&&";
    case OP_OR:            return "||";
    default:               return "<op>";
  }
}


static const char *node_kind_name(node_kind_t kind) {
  switch (kind) {
    case NODE_NUMBER:       return "IntegerLiteral";
    case NODE_STRING:       return "StringLiteral";
    case NODE_IDENTIFIER:   return "Identifier";
    case NODE_BINOP:        return "BinaryOperator";
    case NODE_UNARY:        return "UnaryOperator";
    case NODE_CALL:         return "CallExpr";
    case NODE_DEREF:        return "PtrDeref";
    case NODE_CAST:         return "CStyleCastExpr";
    case NODE_VAR_DECL:     return "VarDecl";
    case NODE_ASSIGN:       return "AssignStmt";
    case NODE_RETURN:       return "ReturnStmt";
    case NODE_IF:           return "IfStmt";
    case NODE_WHILE:        return "WhileStmt";
    case NODE_FOR:          return "ForStmt";
    case NODE_BLOCK:        return "CompoundStmt";
    case NODE_FUNCTION:     return "FunctionDecl";
    case NODE_REG_DECL:     return "RegDecl";
    case NODE_GLOBAL_VAR:   return "GlobalVarDecl";
    case NODE_STRUCT_DECL:  return "StructDecl";
    case NODE_STRUCT_INIT:  return "StructInitExpr";
    case NODE_FIELD_ACCESS: return "FieldAccess";
    case NODE_PROGRAM:      return "TranslationUnitDecl";
    default:                return "Unknown";
  }
}


static void print_type_suffix(type_t type) {
  if (type.kind == TYPE_STRUCT) {
    printf("%s", type.struct_name ? type.struct_name : "<anon struct>");
  } else {
    printf("%s", type_name(type.kind));
  }
  for (unsigned i = 0; i < type.ptr_depth; i++) {  // one '*' per level (u16** not u16*)
    putchar('*');
  }
}


static void print_ast_label(node_t *node) {
  if (!node) {
    printf("<null>");
    return;
  }

  switch (node->kind) {
    case NODE_NUMBER:
      printf("%s (%ld)", node_kind_name(node->kind), node->number);
      break;
    case NODE_STRING:
      printf("%s \"%s\"", node_kind_name(node->kind), node->value);
      break;
    case NODE_IDENTIFIER:
      printf("%s %s", node_kind_name(node->kind), node->identifier.name ? node->identifier.name : "<anon>");
      break;
    case NODE_BINOP:
      printf("%s %s", node_kind_name(node->kind), op_name(node->binop.op));
      break;
    case NODE_UNARY:
      printf("%s %s", node_kind_name(node->kind), op_name(node->unary.op));
      break;
    case NODE_CALL:
      printf("%s %s", node_kind_name(node->kind), node->call.name ? node->call.name : "<call>");
      break;
    case NODE_DEREF:
      printf("%s *", node_kind_name(node->kind));
      break;
    case NODE_FIELD_ACCESS:
      printf("%s .%s", node_kind_name(node->kind), node->field_access.field ? node->field_access.field : "<anon>");
      break;
    case NODE_STRUCT_INIT:
      printf("%s %s", node_kind_name(node->kind), node->struct_init.struct_name ? node->struct_init.struct_name : "<anon>");
      break;
    case NODE_CAST:
      printf("%s ", node_kind_name(node->kind));
      print_type_suffix(node->cast.cast_type);
      break;
    case NODE_VAR_DECL:
      printf("%s %s : ", node_kind_name(node->kind), node->var_decl.name ? node->var_decl.name : "<anon>");
      print_type_suffix(node->var_decl.type);
      break;
    case NODE_ASSIGN:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_RETURN:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_IF:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_WHILE:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_FOR:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_BLOCK:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_FUNCTION:
      printf("%s %s(", node_kind_name(node->kind), node->function.name ? node->function.name : "<anon>");
      for (unsigned i = 0; i < node->function.params.count; ++i) {
        param_t *param = &node->function.params.items[i];
        if (i > 0) printf(", ");
        print_type_suffix(param->type);
        printf(" %s", param->name ? param->name : "<anon>");
      }
      printf(") -> ");
      print_type_suffix(node->function.return_type);
      break;
    case NODE_REG_DECL:
      printf("%s %s : ", node_kind_name(node->kind), node->reg_decl.name ? node->reg_decl.name : "<anon>");
      print_type_suffix(node->reg_decl.type);
      printf(" @ %3lx", node->reg_decl.addr);
      break;
    case NODE_GLOBAL_VAR:
      printf("%s %s : ", node_kind_name(node->kind), node->global_var.name ? node->global_var.name : "<anon>");
      print_type_suffix(node->global_var.type);
      break;
    case NODE_STRUCT_DECL:
      printf("%s %s {", node_kind_name(node->kind), node->struct_decl.name ? node->struct_decl.name : "<anon>");
      for (unsigned i = 0; i < node->struct_decl.fields.count; ++i) {
        field_t *field = &node->struct_decl.fields.items[i];
        if (i > 0) printf(", ");
        print_type_suffix(field->type);
        printf(" %s", field->name ? field->name : "<anon>");
      }
      printf("}");
      break;
    case NODE_PROGRAM:
      printf("%s", node_kind_name(node->kind));
      break;
    default:
      printf("%s", node_kind_name(node->kind));
      break;
  }
}


static void print_ast_list(node_list_t list, const char *prefix) {
  for (unsigned i = 0; i < list.count; ++i) {
    print_ast_(list.items[i], (i + 1 == list.count), prefix);
  }
}


static void print_ast_labeled(const char *label, node_t *node, int is_last, const char *prefix) {
  printf("%s%s%s\n", prefix, is_last ? "`- " : "|- ", label);

  char child_prefix[AST_PRINT_MAX_DEPTH];
  if (strlen(prefix) + 4 >= AST_PRINT_MAX_DEPTH) {  // same depth guard as print_ast_
    return;
  }
  strcpy(child_prefix, prefix);
  strcat(child_prefix, is_last ? "   " : "|  ");

  if (node) print_ast_(node, 1, child_prefix);
}


static void print_ast_(node_t *node, int is_last, const char *prefix) {
  if (!node) {
    printf("<null>\n");
    return;
  }

  printf("%s%s", prefix, is_last ? "`- " : "|- ");
  print_ast_label(node);
  putchar('\n');

  char child_prefix[AST_PRINT_MAX_DEPTH];
  if (strlen(prefix) + 4 >= AST_PRINT_MAX_DEPTH) {
    return;
  }

  strcpy(child_prefix, prefix);
  strcat(child_prefix, is_last ? "   " : "|  ");

  switch (node->kind) {
    case NODE_NUMBER:
    case NODE_STRING:
    case NODE_IDENTIFIER:
      break;

    case NODE_BINOP:
      print_ast_(node->binop.left, 0, child_prefix);
      print_ast_(node->binop.right, 1, child_prefix);
      break;

    case NODE_UNARY:
      print_ast_(node->unary.operand, 1, child_prefix);
      break;

    case NODE_CALL:
      // args are always the last (and only) children of a call
      print_ast_list(node->call.args, child_prefix);
      break;

    case NODE_DEREF:
      print_ast_(node->deref_target, 1, child_prefix);
      break;

    case NODE_FIELD_ACCESS:
      print_ast_(node->field_access.base, 1, child_prefix);
      break;

    case NODE_CAST:
      print_ast_(node->cast.operand, 1, child_prefix);
      break;

    case NODE_VAR_DECL:
      if (node->var_decl.initialiser) {
        print_ast_(node->var_decl.initialiser, 1, child_prefix);
      }
      break;

    case NODE_ASSIGN:
      print_ast_labeled("[target]", node->assign.target, 0, child_prefix);
      print_ast_labeled("[value]", node->assign.value, 1, child_prefix);
      break;

    case NODE_RETURN:
      if (node->return_val) {
        print_ast_(node->return_val, 1, child_prefix);
      }
      break;

    case NODE_IF: {
      print_ast_labeled("[cond]", node->if_stmt.cond, 0, child_prefix);

      unsigned has_else = node->if_stmt.blocks.count > 1;
      printf("%s%s[then body]\n", child_prefix, has_else ? "|- " : "`- ");

      char body_prefix[AST_PRINT_MAX_DEPTH];
      strcpy(body_prefix, child_prefix);
      strcat(body_prefix, has_else ? "|  " : "   ");
      print_ast_(node->if_stmt.blocks.items[0], has_else, body_prefix);

      for (unsigned i = 1; i < node->if_stmt.blocks.count; ++i) {
        node_t *block = node->if_stmt.blocks.items[i];
        unsigned has_more_blocks = i < node->if_stmt.blocks.count  - 1;

        char else_prefix[AST_PRINT_MAX_DEPTH];
        strcpy(else_prefix, child_prefix);
        strcat(else_prefix, "   ");
        print_ast_(block, !has_more_blocks, else_prefix);
      }

      break;
    }

    case NODE_WHILE: {
      print_ast_labeled("[cond]", node->while_stmt.cond, 0, child_prefix);

      if (node->while_stmt.body) {
        printf("%s`- [body]\n", child_prefix);
        char body_prefix[AST_PRINT_MAX_DEPTH];
        strcpy(body_prefix, child_prefix);
        strcat(body_prefix, "   ");
        print_ast_(node->while_stmt.body, 1, body_prefix);
      }

      break;
    }

    case NODE_FOR: {
      int has_body = node->for_stmt.body != NULL;
      if (node->for_stmt.initialiser)
        print_ast_labeled("[init]", node->for_stmt.initialiser, 0, child_prefix);
      if (node->for_stmt.cond)
        print_ast_labeled("[cond]", node->for_stmt.cond, 0, child_prefix);
      if (node->for_stmt.incrementer)
        print_ast_labeled("[step]", node->for_stmt.incrementer, !has_body, child_prefix);
      if (has_body) {
        printf("%s`- [body]\n", child_prefix);
        char body_prefix[AST_PRINT_MAX_DEPTH];
        strcpy(body_prefix, child_prefix);
        strcat(body_prefix, "   ");
        print_ast_(node->for_stmt.body, 1, body_prefix);
      }
      break;
    }

    case NODE_BLOCK:
      print_ast_list(node->block, child_prefix);
      break;

    case NODE_FUNCTION:
      print_ast_(node->function.body, 1, child_prefix);
      break;

    case NODE_REG_DECL:
      break;

    case NODE_STRUCT_DECL:
      break;

    case NODE_STRUCT_INIT: {
      for (unsigned i = 0; i < node->struct_init.inits.count; ++i) {
        field_init_t *init = &node->struct_init.inits.items[i];
        unsigned is_last_init = (i + 1 == node->struct_init.inits.count);
 
        char label[64];
        snprintf(label, sizeof(label), "[.%s]", init->field_name ? init->field_name : "<anon>");
        print_ast_labeled(label, init->value, is_last_init, child_prefix);
      }
      break;
    }

    case NODE_GLOBAL_VAR:
      if (node->global_var.initialiser) {
        print_ast_(node->global_var.initialiser, 1, child_prefix);
      }
      break;

    case NODE_PROGRAM:
      print_ast_list(node->program, child_prefix);
      break;
  }
}


void print_ast(ast_t node) {
  char prefix[AST_PRINT_MAX_DEPTH] = "";
  print_ast_(node, 1, prefix);
}