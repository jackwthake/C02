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
}
