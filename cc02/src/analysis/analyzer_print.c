#include "analyzer.h"

#include <stdio.h>

#include "colors.h"


static const char *type_name(type_kind_t kind) {
  switch (kind) {
    case TYPE_U8:     return "u8";
    case TYPE_I8:     return "i8";
    case TYPE_U16:    return "u16";
    case TYPE_I16:    return "i16";
    case TYPE_VOID:   return "void";
    case TYPE_STRUCT: return "struct";
    default:          return "<unknown>";
  }
}


static void print_type(type_t type) {
  if (type.kind == TYPE_STRUCT) {
    printf("%s", type.struct_name ? type.struct_name : "<anon>");
  } else {
    printf("%s", type_name(type.kind));
  }

  for (unsigned i = 0; i < type.ptr_depth; i++) {
    putchar('*');
  }
}


static const char *symbol_kind_name(symbol_kind_t kind) {
  switch (kind) {
    case SYMBOL_VARIABLE: return "variable";
    case SYMBOL_FUNCTION: return "function";
    case SYMBOL_STRUCT:   return "struct";
    default:              return "<unknown>";
  }
}


static void print_symbol(symbol_t *sym) {
  printf("  %s%-10s" RESET " %s", CYAN, sym->name, symbol_kind_name(sym->kind));

  switch (sym->kind) {
    case SYMBOL_VARIABLE:
      printf(" : ");
      print_type(sym->variable.type);
      if (sym->variable.is_register) {
        printf(" @ 0x%lx", sym->variable.addr);
      }
      break;

    case SYMBOL_FUNCTION:
      printf(" (");
      for (unsigned i = 0; i < sym->function.params.count; i++) {
        if (i > 0) printf(", ");
        print_type(sym->function.params.items[i].type);
        printf(" %s", sym->function.params.items[i].name);
      }
      printf(") -> ");
      print_type(sym->function.return_type);
      break;

    case SYMBOL_STRUCT:
      printf(" { ");
      for (unsigned i = 0; i < sym->struct_decl.fields.count; i++) {
        if (i > 0) printf(", ");
        print_type(sym->struct_decl.fields.items[i].type);
        printf(" %s", sym->struct_decl.fields.items[i].name);
      }
      printf(" }");
      break;
  }

  putchar('\n');
}


void print_symtab(symtab_t *table) {
  for (unsigned i = 0; i < SYMTAB_NUM_BUCKETS; i++) {
    for (symtab_entry_t *e = table->buckets[i]; e; e = e->next) {
      print_symbol(&e->value);
    }
  }
}
