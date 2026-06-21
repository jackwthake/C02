#include "errors.h"

#include <stdio.h>
#include <stdlib.h>

#include "colors.h"


// Renders `type` into `buf` (e.g. "u8", "u16*", "Point*"). Returns `buf`,
// so this can be used directly as an fprintf argument:
//   char b[64];
//   fprintf(stderr, "%s", type_to_string(type, b, sizeof(b)));
// A caller-supplied buffer (rather than a static one inside this function)
// is deliberate: two calls in the same fprintf argument list would
// otherwise silently stomp on each other if they shared one static buffer.
static char *type_to_string(type_t type, char *buf, size_t buf_size) {
  const char *base;
  switch (type.kind) {
    case TYPE_U8:     base = "u8";   break;
    case TYPE_I8:     base = "i8";   break;
    case TYPE_U16:    base = "u16";  break;
    case TYPE_I16:    base = "i16";  break;
    case TYPE_VOID:   base = "void"; break;
    case TYPE_STRUCT: base = type.struct_name ? type.struct_name : "<anon struct>"; break;
    default:          base = "<unknown type>"; break;
  }

  // one '*' per pointer level so a u16** mismatch doesn't print as just u16*
  char stars[8];
  unsigned n = type.ptr_depth < sizeof(stars) - 1 ? type.ptr_depth : (unsigned)(sizeof(stars) - 1);
  for (unsigned i = 0; i < n; ++i) stars[i] = '*';
  stars[n] = '\0';

  snprintf(buf, buf_size, "%s%s", base, stars);
  return buf;
}


#define PRINT_ERR_HEADER(e)                                                              \
  fprintf(stderr, BOLD_WHITE "%s" RESET ":" BOLD_WHITE "%u" RESET ":" BOLD_WHITE "%u: "  \
              BOLD_RED "error: " RESET, (e)->loc.file_path, (e)->loc.line, (e)->loc.column)


static void print_parse_kind_error(error_t *e) {
  const token_t *tok = &e->parse.found;

  switch (e->type) {
    case ERR_UNEXPECTED_EOF:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "unexpected end of file\n");
      fprintf(stderr, "  =" BOLD_BLUE " expected: " RESET BLUE "%s\n" RESET, e->parse.expected);
      fprintf(stderr, "  =" BOLD_BLUE " context:  " RESET BLUE "%s\n" RESET, e->parse.context);
      print_error_line(e->loc);
      return;

    case ERR_ALLOCATION_FAILED:
      fprintf(stderr, BOLD_BLUE "[c02 Internal] " RESET);
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "Internal parsing allocation failed - this is a bug, please report it.\n");
      return;

    case ERR_UNEXPECTED_TOKEN: {
      if (token_has_value(tok->type)) {
        unsigned should_free = 0;
        char *val = token_val_to_string(*tok, &should_free);
        PRINT_ERR_HEADER(e);
        fprintf(stderr, "unexpected token: %s\n", val);
        fprintf(stderr, "  =" BOLD_BLUE " expected: " RESET BLUE "%s\n" RESET, e->parse.expected);
        fprintf(stderr, "  =" BOLD_BLUE " context:  " RESET BLUE "%s\n" RESET, e->parse.context);
        print_error_line(e->loc);
        if (should_free) free(val);
      } else {
        PRINT_ERR_HEADER(e);
        fprintf(stderr, "unexpected token\n");
        fprintf(stderr, "  =" BOLD_BLUE " expected: " RESET BLUE "%s\n" RESET, e->parse.expected);
        fprintf(stderr, "  =" BOLD_BLUE " context:  " RESET BLUE "%s\n" RESET, e->parse.context);
        print_error_line(e->loc);
      }
      return;
    }

    default:
      return; // unreachable - caller dispatches on type before getting here
  }
}


static void print_semantic_kind_error(error_t *e) {
  char expected_buf[64], actual_buf[64];

  switch (e->type) {
    case ERR_MISSING_MAIN:
      // a whole-translation-unit error: there is no meaningful source span to
      // point a caret at, so print just the file and message - no snippet.
      fprintf(stderr, BOLD_WHITE "%s" RESET ": " BOLD_RED "error: " RESET
              "missing or improperly defined main function\n", e->loc.file_path);
      return;

    case ERR_UNDECLARED_IDENTIFIER:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "undeclared identifier '%s'\n", e->name_error.name);
      break;

    case ERR_NOT_A_FUNCTION:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "'%s' is not a function\n", e->name_error.name);
      break;

    case ERR_UNKNOWN_STRUCT:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "unknown struct '%s'\n", e->name_error.name);
      break;

    case ERR_REDECLARATION:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "redeclaration of '%s' in this scope\n", e->name_error.name);
      break;

    case ERR_NOT_ASSIGNABLE:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "'%s' is not assignable\n", e->name_error.name);
      break;

    case ERR_TYPE_MISMATCH:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "type mismatch in %s: expected %s, found %s\n",
              e->type_mismatch.context,
              type_to_string(e->type_mismatch.expected, expected_buf, sizeof(expected_buf)),
              type_to_string(e->type_mismatch.actual, actual_buf, sizeof(actual_buf)));
      break;

    case ERR_WRONG_ARG_TYPE:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "type mismatch in %s: expected %s, found %s\n",
              e->type_mismatch.context,
              type_to_string(e->type_mismatch.expected, expected_buf, sizeof(expected_buf)),
              type_to_string(e->type_mismatch.actual, actual_buf, sizeof(actual_buf)));
      break;

    case ERR_WRONG_ARG_COUNT:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "function '%s' expects %u argument(s), but %u were provided\n",
              e->arg_count.fn_name, e->arg_count.expected_count, e->arg_count.actual_count);
      break;

    case ERR_UNKNOWN_FIELD:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "struct '%s' has no field '%s'\n",
              e->unknown_field.struct_name, e->unknown_field.field_name);
      break;

    case ERR_LITERAL_OUT_OF_RANGE:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "integer literal is out of range (must fit in -32768..65535)\n");
      break;

    case ERR_NOT_LVALUE:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "%s\n", e->lvalue.message);
      break;

    case ERR_VOID_VARIABLE:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "'%s' declared with incomplete type 'void'\n", e->name_error.name);
      break;

    case ERR_NOT_A_STRUCT:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "request for field '%s' in '%s', which is not a struct\n",
              e->type_mismatch.context,
              type_to_string(e->type_mismatch.actual, actual_buf, sizeof(actual_buf)));
      break;

    case ERR_MISSING_RETURN:
      PRINT_ERR_HEADER(e);
      fprintf(stderr, "non-void function '%s' may reach its end without returning a value\n",
              e->name_error.name);
      break;

    default:
      return; // unreachable - parser kinds handled in print_parse_kind_error
  }

  print_error_line(e->loc);
}


void print_error(error_t *e) {
  switch (e->type) {
    case ERR_UNEXPECTED_EOF:
    case ERR_UNEXPECTED_TOKEN:
    case ERR_ALLOCATION_FAILED:
      print_parse_kind_error(e);
      return;

    default:
      print_semantic_kind_error(e);
      return;
  }
}