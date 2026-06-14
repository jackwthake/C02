#include "parser.h"

#include <string.h>
#include <stdio.h>

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
    case OP_PLUS:          return "+";
    case OP_MINUS:         return "-";
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
    default:               return "<op>";
  }
}


static const char *node_kind_name(node_kind_t kind) {
  switch (kind) {
    case NODE_NUMBER:       return "IntegerLiteral";
    case NODE_IDENTIFIER:   return "DeclRefExpr";
    case NODE_BINOP:        return "BinaryOperator";
    case NODE_UNARY:        return "UnaryOperator";
    case NODE_CALL:         return "CallExpr";
    case NODE_DEREF:        return "UnaryOperator";
    case NODE_CAST:         return "CStyleCastExpr";
    case NODE_VAR_DECL:     return "VarDecl";
    case NODE_ASSIGN:       return "BinaryOperator";
    case NODE_DEREF_ASSIGN: return "CompoundAssignOperator";
    case NODE_RETURN:       return "ReturnStmt";
    case NODE_IF:           return "IfStmt";
    case NODE_WHILE:        return "WhileStmt";
    case NODE_EXPR_STMT:    return "ExprStmt";
    case NODE_FUNCTION:     return "FunctionDecl";
    case NODE_REG_DECL:     return "RegDecl";
    case NODE_GLOBAL_VAR:   return "GlobalVarDecl";
    case NODE_PROGRAM:      return "TranslationUnitDecl";
    default:                return "Unknown";
  }
}


static void print_type_suffix(type_t type) {
  printf("%s%s", type_name(type.kind), type.is_ptr ? "*" : "");
}


static void print_ast_label(node_t *node) {
  if (!node) {
    printf("<null>");
    return;
  }

  switch (node->kind) {
    case NODE_NUMBER:
      printf("%s (%d)", node_kind_name(node->kind), node->number);
      break;
    case NODE_IDENTIFIER:
      printf("%s %s", node_kind_name(node->kind), node->identifier ? node->identifier : "<anon>");
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
    case NODE_CAST:
      printf("%s ", node_kind_name(node->kind));
      print_type_suffix(node->cast.cast_type);
      break;
    case NODE_VAR_DECL:
      printf("%s %s : ", node_kind_name(node->kind), node->var_decl.name ? node->var_decl.name : "<anon>");
      print_type_suffix(node->var_decl.type);
      break;
    case NODE_ASSIGN:
      printf("%s %s", node_kind_name(node->kind), node->assign.name ? node->assign.name : "<anon>");
      break;
    case NODE_DEREF_ASSIGN:
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
    case NODE_EXPR_STMT:
      printf("%s", node_kind_name(node->kind));
      break;
    case NODE_FUNCTION:
      printf("%s %s : ", node_kind_name(node->kind), node->function.name ? node->function.name : "<anon>");
      print_type_suffix(node->function.return_type);
      break;
    case NODE_REG_DECL:
      printf("%s %s : ", node_kind_name(node->kind), node->reg_decl.name ? node->reg_decl.name : "<anon>");
      print_type_suffix(node->reg_decl.type);
      break;
    case NODE_GLOBAL_VAR:
      printf("%s %s : ", node_kind_name(node->kind), node->global_var.name ? node->global_var.name : "<anon>");
      print_type_suffix(node->global_var.type);
      break;
    case NODE_PROGRAM:
      printf("%s", node_kind_name(node->kind));
      break;
    default:
      printf("%s", node_kind_name(node->kind));
      break;
  }
}


static void print_ast_list(node_list_t list, const char *prefix, int is_last) {
  for (unsigned i = 0; i < list.count; ++i) {
    char child_prefix[AST_PRINT_MAX_DEPTH];

    if (strlen(prefix) + 4 >= AST_PRINT_MAX_DEPTH) {
      return;
    }

    strcpy(child_prefix, prefix);
    strcat(child_prefix, is_last ? "   " : "|  ");

    print_ast_(list.items[i], (i + 1 == list.count), child_prefix);
  }
}


static void print_ast_(node_t *node, int is_last, const char *prefix) {
  if (!node) {
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
      print_ast_list(node->call.args, child_prefix, is_last);
      break;

    case NODE_DEREF:
      print_ast_(node->deref_target, 1, child_prefix);
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
      print_ast_(node->assign.value, 1, child_prefix);
      break;

    case NODE_DEREF_ASSIGN:
      print_ast_(node->deref_assign.target, 0, child_prefix);
      print_ast_(node->deref_assign.value, 1, child_prefix);
      break;

    case NODE_RETURN:
      if (node->return_val) {
        print_ast_(node->return_val, 1, child_prefix);
      }
      break;

    case NODE_IF:
      print_ast_(node->if_stmt.cond, 0, child_prefix);
      print_ast_list(node->if_stmt.then_block, child_prefix, 0);
      print_ast_list(node->if_stmt.else_block, child_prefix, 1);
      break;

    case NODE_WHILE:
      print_ast_(node->while_stmt.cond, 0, child_prefix);
      print_ast_list(node->while_stmt.body, child_prefix, 1);
      break;

    case NODE_EXPR_STMT:
      if (node->expr_stmt) {
        print_ast_(node->expr_stmt, 1, child_prefix);
      }
      break;

    case NODE_FUNCTION:
      print_ast_list(node->function.body, child_prefix, is_last);
      break;

    case NODE_REG_DECL:
    case NODE_GLOBAL_VAR:
      if (node->global_var.initialiser) {
        print_ast_(node->global_var.initialiser, 1, child_prefix);
      }
      break;

    case NODE_PROGRAM:
      print_ast_list(node->program, child_prefix, is_last);
      break;
  }
}


void print_ast(node_t *node) {
  char prefix[AST_PRINT_MAX_DEPTH] = "";
  print_ast_(node, 1, prefix);
}
