#include "parser.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "colors.h"

#define AST_PRINT_MAX_DEPTH 128

void print_parse_error(error_t *e);
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
    case NODE_DEREF_ASSIGN: return "CompoundAssignOperator";
    case NODE_RETURN:       return "ReturnStmt";
    case NODE_IF:           return "IfStmt";
    case NODE_WHILE:        return "WhileStmt";
    case NODE_FOR:          return "ForStmt";
    case NODE_BLOCK:        return "CompoundStmt";
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
      printf("%s (%ld)", node_kind_name(node->kind), node->number);
      break;
    case NODE_STRING:
      printf("%s \"%s\"", node_kind_name(node->kind), node->value);
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
      printf("%s %s =", node_kind_name(node->kind), node->assign.name ? node->assign.name : "<anon>");
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
        printf("%s%s %s", type_name(param->type.kind), param->type.is_ptr ? "*" : "", param->name ? param->name : "<anon>");
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

    case NODE_IF: {
      print_ast_labeled("[cond]", node->if_stmt.cond, 0, child_prefix);

      unsigned has_else = node->if_stmt.blocks.count > 1;
      printf("%s%s[then body]\n", child_prefix, has_else ? "|- " : "`- ");

      char body_prefix[AST_PRINT_MAX_DEPTH];
      strcpy(body_prefix, child_prefix);
      strcat(body_prefix, has_else ? "|  " : "   ");
      print_ast_(node->if_stmt.blocks.items[0], !has_else, body_prefix);

      for (unsigned i = 1; i < node->if_stmt.blocks.count; ++i) {
        node_t *block = node->if_stmt.blocks.items[i];
        unsigned has_more_blocks = i < node->if_stmt.blocks.count - 1;

        if (block->kind == NODE_IF) {
          printf("%s%s[else if body]\n", child_prefix, has_more_blocks ? "|- " : "`- ");
        } else {
          printf("%s%s[else body]\n", child_prefix, has_more_blocks ? "|- " : "`- ");
        }

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


void print_ast(node_t *node) {
  char prefix[AST_PRINT_MAX_DEPTH] = "";
  print_ast_(node, 1, prefix);
}


#define PRINT_ERR_HEADER_(err_string, val)                                                \
  do {                                                                                    \
  fprintf(stderr, BOLD_WHITE "%s" RESET ":" BOLD_WHITE "%u" RESET ":" BOLD_WHITE "%u: "   \
              BOLD_RED "error: " RESET err_string "\n",                                   \
              tok->file_path, tok->line, tok->column, val);                               \
  } while (0);

#define PRINT_ERR_HEADER(err_string) \
    PRINT_ERR_HEADER_(err_string, "")

#define PRINT_ERROR_MESG_VAL(err_string, val)                                             \
  do {                                                                                    \
    PRINT_ERR_HEADER_(err_string, val);                                                   \
    fprintf(stderr, "  =" BOLD_BLUE " expected: " RESET BLUE "%s\n" RESET, e->expected);  \
    fprintf(stderr, "  =" BOLD_BLUE " context:  " RESET BLUE "%s\n" RESET, e->context);   \
  } while (0)

#define PRINT_ERROR_MESG(err_string) \
  PRINT_ERROR_MESG_VAL(err_string, "")

void print_parse_error(error_t *e) {
  const token_t *tok = &e->found;

  switch (e->type) {
    case UNEXPECTED_EOF:
      PRINT_ERROR_MESG("unexpected end of file");
      print_error_line(tok->line, tok->column, 1);
      return;
    
    case ALLOCATION_FAILED: {
      fprintf(stderr, BOLD_BLUE "[c02 Internal] " RESET );
      PRINT_ERR_HEADER("Internal parsing allocation failed - this is a bug, please report it."); // will print c02 internal file name and not user's c02 code
      return;
    }

    case UNEXPECTED_TOKEN: {
      if (token_has_value(tok->type)) {
        unsigned should_free = 0;
        char *val = token_val_to_string(*tok, &should_free);
        PRINT_ERROR_MESG_VAL("unexpected token: %s", val);
        print_error_line(tok->line, tok->column, tok->length);
        if (should_free) free(val);
      } else {
        PRINT_ERROR_MESG("unexpected token");
        print_error_line(tok->line, tok->column, tok->length);
      }
      return;
    }
  }
}
