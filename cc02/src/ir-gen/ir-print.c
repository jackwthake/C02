#include "ir.h"

#include <stdio.h>


static const char *type_kind_str(type_kind_t k) {
  switch (k) {
    case TYPE_U8:      return "u8";
    case TYPE_I8:      return "i8";
    case TYPE_U16:     return "u16";
    case TYPE_I16:     return "i16";
    case TYPE_VOID:    return "void";
    case TYPE_STRUCT:  return "struct";
    case TYPE_INVALID: return "<invalid>";
  }
  return "?";
}

static void print_type(type_t t) {
  if (t.kind == TYPE_STRUCT)
    printf("%s", t.struct_name);
  else
    printf("%s", type_kind_str(t.kind));
  for (unsigned i = 0; i < t.ptr_depth; i++)
    printf("*");
}

static void print_operand(tac_operand_t op) {
  switch (op.kind) {
    case OPERAND_NONE:      printf("_"); break;
    case OPERAND_TEMP:      printf("t%u", op.temp_id); break;
    case OPERAND_VAR:       printf("%s", op.name); break;
    case OPERAND_CONST_INT: printf("%ld", op.int_val); break;
    case OPERAND_CONST_STR: printf("\"%s\"", op.str_val); break;
  }
}

static const char *tac_op_str(tac_op_t op) {
  switch (op) {
    case TAC_ADD:  return "+";   case TAC_SUB:  return "-";
    case TAC_MUL:  return "*";   case TAC_DIV:  return "/";
    case TAC_MOD:  return "%";
    case TAC_LT:   return "<";   case TAC_GT:   return ">";
    case TAC_LTE:  return "<=";  case TAC_GTE:  return ">=";
    case TAC_EQ:   return "==";  case TAC_NEQ:  return "!=";
    case TAC_AND:  return "&&";  case TAC_OR:   return "||";
    case TAC_SHL:  return "<<";  case TAC_SHR:  return ">>";
    case TAC_BAND: return "&";   case TAC_BXOR: return "^";
    case TAC_BOR:  return "|";
    default:       return "?";
  }
}

static void print_instr(tac_instr_t *ins) {
  switch (ins->op) {
    case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV: case TAC_MOD:
    case TAC_LT: case TAC_GT: case TAC_LTE: case TAC_GTE:
    case TAC_EQ: case TAC_NEQ:
    case TAC_AND: case TAC_OR:
    case TAC_SHL: case TAC_SHR:
    case TAC_BAND: case TAC_BXOR: case TAC_BOR:
      print_operand(ins->dst);
      printf(" = ");
      print_operand(ins->src1);
      printf(" %s ", tac_op_str(ins->op));
      print_operand(ins->src2);
      break;

    case TAC_NEG:
      print_operand(ins->dst);
      printf(" = -");
      print_operand(ins->src1);
      break;

    case TAC_NOT:
      print_operand(ins->dst);
      printf(" = !");
      print_operand(ins->src1);
      break;

    case TAC_BNOT:
      print_operand(ins->dst);
      printf(" = ~");
      print_operand(ins->src1);
      break;

    case TAC_INC:
      printf("inc ");
      print_operand(ins->dst);
      break;

    case TAC_DEC:
      printf("dec ");
      print_operand(ins->dst);
      break;

    case TAC_ADDR_OF:
      print_operand(ins->dst);
      printf(" = &");
      print_operand(ins->src1);
      break;

    case TAC_LOAD:
      print_operand(ins->dst);
      printf(" = *");
      if (ins->src1.kind == OPERAND_CONST_INT)
        printf("0x%04lX", ins->src1.int_val);
      else
        print_operand(ins->src1);
      break;

    case TAC_STORE:
      printf("*");
      if (ins->dst.kind == OPERAND_CONST_INT)
        printf("0x%04lX", ins->dst.int_val);
      else
        print_operand(ins->dst);
      printf(" = ");
      print_operand(ins->src1);
      break;

    case TAC_COPY:
      print_operand(ins->dst);
      printf(" = ");
      print_operand(ins->src1);
      break;

    case TAC_LABEL:
      printf("L%u:", ins->label);
      break;

    case TAC_JUMP:
      printf("goto L%u", ins->label);
      break;

    case TAC_COND_JUMP:
      printf("if ");
      print_operand(ins->src1);
      printf(" goto L%u", ins->label);
      break;

    case TAC_CALL:
      print_operand(ins->dst);
      printf(" = call %s(", ins->call_name);
      for (unsigned i = 0; i < ins->call_arg_count; i++) {
        if (i > 0) printf(", ");
        print_operand(ins->call_args[i]);
      }
      printf(")");
      break;

    case TAC_RETURN:
      printf("return");
      if (ins->src1.kind != OPERAND_NONE) {
        printf(" ");
        print_operand(ins->src1);
      }
      break;
    
    case TAC_BREAK:
      printf("break");
      break;

    case TAC_CONTINUE:
      printf("continue");
      break;

    case TAC_FIELD_LOAD:
      print_operand(ins->dst);
      printf(" = ");
      print_operand(ins->src1);
      printf(".%s", ins->field_name);
      break;

    case TAC_FIELD_STORE:
      print_operand(ins->dst);
      printf(".%s = ", ins->field_name);
      print_operand(ins->src1);
      break;

    case TAC_CAST:
      print_operand(ins->dst);
      printf(" = (");
      print_type(ins->cast_type);
      printf(")");
      print_operand(ins->src1);
      break;

    case TAC_ASM:
      printf("asm { ");
      for (unsigned i = 0; i < ins->asm_mnemonic_count; i++) {
        if (i > 0) printf("; ");
        printf("%s", ins->asm_mnemonics[i]);
      }
      printf(" }");
      break;
  }
  printf("\n");
}

static void print_cfg(cfg_t *cfg) {
  if (cfg->is_interrupt) printf("interrupt ");
  printf("fn %s(", cfg->name);
  for (unsigned i = 0; i < cfg->params.count; i++) {
    if (i > 0) printf(", ");
    print_type(cfg->params.items[i].type);
    printf(" %s", cfg->params.items[i].name);
  }
  printf(") -> ");
  print_type(cfg->return_type);
  printf(" {\n");

  for (unsigned i = 0; i < cfg->block_count; i++) {
    basic_block_t *bb = cfg->blocks[i];
    printf("block%u:\n", bb->id);
    for (unsigned j = 0; j < bb->instr_count; j++) {
      printf("  ");
      print_instr(&bb->instrs[j]);
    }
  }
  printf("}\n\n");
}

void ir_gen_print(ir_gen_t *gen) {
  ir_module_t *m = &gen->module;
  printf("\n");

  if (m->reg_count > 0) {
    printf("--- Registers ---\n");
    for (unsigned i = 0; i < m->reg_count; i++) {
      printf("  reg ");
      print_type(m->regs[i].type);
      printf(" %s @ 0x%04lX\n", m->regs[i].name, m->regs[i].addr);
    }
    printf("\n");
  }

  if (m->global_count > 0) {
    printf("--- Globals ---\n");
    for (unsigned i = 0; i < m->global_count; i++) {
      printf("  ");
      print_type(m->globals[i].type);
      printf(" %s", m->globals[i].name);
      switch (m->globals[i].init_kind) {
        case IR_INIT_INT: printf(" = %ld", m->globals[i].int_val); break;
        case IR_INIT_STR: printf(" = \"%s\"", m->globals[i].str_val); break;
        case IR_INIT_NONE: break;
      }
      printf("\n");
    }
    printf("\n");
  }

  if (m->struct_count > 0) {
    printf("--- Structs ---\n");
    for (unsigned i = 0; i < m->struct_count; i++) {
      ir_struct_def_t *s = &m->structs[i];
      printf("  struct %s (%u bytes)\n", s->name, s->total_size);
      for (unsigned j = 0; j < s->field_count; j++) {
        printf("    +%u  ", s->fields[j].offset);
        print_type(s->fields[j].type);
        printf(" %s\n", s->fields[j].name);
      }
    }
    printf("\n");
  }

  if (m->extern_count > 0) {
    printf("--- Externs ---\n");
    for (unsigned i = 0; i < m->extern_count; i++) {
      ir_extern_t *e = &m->externs[i];
      if (e->is_function) {
        printf("  decl fn %s(", e->name);
        for (unsigned j = 0; j < e->params.count; j++) {
          if (j > 0) printf(", ");
          print_type(e->params.items[j].type);
          printf(" %s", e->params.items[j].name);
        }
        printf(") -> ");
        print_type(e->type);
        printf("\n");
      } else {
        printf("  decl ");
        print_type(e->type);
        printf(" %s\n", e->name);
      }
    }
    printf("\n");
  }

  if (m->cfg_count > 0) {
    printf("--- Functions ---\n");
    for (unsigned i = 0; i < m->cfg_count; i++) {
      print_cfg(&m->cfgs[i]);
    }
  }
}
