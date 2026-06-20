#include <stdio.h>
#include <string.h>

#include "errors.h"

int main(void) {
  // a parser-shaped error (ERR_UNEXPECTED_TOKEN), built the way
  // GENERATE_ERROR would build it
  token_t bad_tok = {
    .type = l_identifier,
    .loc = { .line = 4, .column = 7, .length = 3, .file_path = "smoke.c02" },
    .string_val = "foo",
  };
  error_t parse_err = {
    .type = ERR_UNEXPECTED_TOKEN,
    .loc = bad_tok.loc,
  };
  parse_err.parse.found = bad_tok;
  parse_err.parse.expected = "';' after statement";
  parse_err.parse.context = "expression statement";

  printf("--- parser-shaped error ---\n");
  print_error(&parse_err);

  // a semantic-shaped error (ERR_TYPE_MISMATCH)
  error_t sem_err = {
    .type = ERR_TYPE_MISMATCH,
    .loc = { .line = 10, .column = 3, .length = 1, .file_path = "smoke.c02" },
  };
  sem_err.type_mismatch.expected = (type_t){ .kind = TYPE_U8 };
  sem_err.type_mismatch.actual = (type_t){ .kind = TYPE_U16 };
  sem_err.type_mismatch.context = "variable 'x'";

  printf("--- semantic-shaped error ---\n");
  print_error(&sem_err);

  // ERR_WRONG_ARG_COUNT
  error_t arg_err = {
    .type = ERR_WRONG_ARG_COUNT,
    .loc = { .line = 12, .column = 5, .length = 1, .file_path = "smoke.c02" },
  };
  arg_err.arg_count.fn_name = "lcd_putc";
  arg_err.arg_count.expected_count = 1;
  arg_err.arg_count.actual_count = 2;

  printf("--- arg count error ---\n");
  print_error(&arg_err);

  printf("errors smoke test completed (visual check - confirm no crash, garbled text, or missing fields above)\n");
  return 0;
}