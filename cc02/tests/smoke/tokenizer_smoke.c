#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "tokenizer.h"

static void test_keywords(void) {
  const char *src = "fn reg return struct void if else while for u8 i8 u16 i16";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  token_type_t expected[] = {
    Kw_fn, Kw_reg, Kw_return, Kw_struct, Kw_void,
    Kw_if, Kw_else, Kw_while, Kw_for,
    t_u8, t_i8, t_u16, t_i16, t_eof
  };
  unsigned num_expected = sizeof(expected) / sizeof(expected[0]);
  assert(count == num_expected);
  for (unsigned i = 0; i < num_expected; i++) {
    assert(tokens[i].type == expected[i]);
  }

  free_tokens(tokens, count);
}

static void test_number_formats(void) {
  const char *src = "42 0xFF 0b1010 0x0 0b0";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].type == l_num && tokens[0].num_val == 42);
  assert(tokens[1].type == l_num && tokens[1].num_val == 0xFF);
  assert(tokens[2].type == l_num && tokens[2].num_val == 0b1010);
  assert(tokens[3].type == l_num && tokens[3].num_val == 0x0);
  assert(tokens[4].type == l_num && tokens[4].num_val == 0);
  assert(tokens[5].type == t_eof);

  free_tokens(tokens, count);
}

static void test_special_literals(void) {
  const char *src = "true false null";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].type == l_num && tokens[0].num_val == 1);
  assert(tokens[1].type == l_num && tokens[1].num_val == 0);
  assert(tokens[2].type == l_num && tokens[2].num_val == 0);
  assert(tokens[3].type == t_eof);

  free_tokens(tokens, count);
}

static void test_strings(void) {
  const char *src = "\"hello world\"";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].type == l_string);
  assert(strcmp(tokens[0].string_val, "hello world") == 0);
  assert(tokens[1].type == t_eof);

  free_tokens(tokens, count);

  // escape sequences decode to their bytes, and the character right after an
  // escape is not dropped. Regression: the old length counter was short by one
  // per escape, so strndup truncated one trailing byte for every escape.
  // C02 source below is:  "a\nb\\c\"d"  -> a, newline, b, backslash, c, ", d
  const char *esc_src = "\"a\\nb\\\\c\\\"d\"";
  unsigned esc_count = 0;
  token_t *esc = tokenize("test", esc_src, (long)strlen(esc_src), &esc_count);
  assert(esc != NULL);
  assert(esc[0].type == l_string);
  assert(strcmp(esc[0].string_val, "a\nb\\c\"d") == 0);
  assert(esc[1].type == t_eof);

  free_tokens(esc, esc_count);
}

static void test_identifiers(void) {
  const char *src = "foo _bar baz123 _";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].type == l_identifier && strcmp(tokens[0].string_val, "foo") == 0);
  assert(tokens[1].type == l_identifier && strcmp(tokens[1].string_val, "_bar") == 0);
  assert(tokens[2].type == l_identifier && strcmp(tokens[2].string_val, "baz123") == 0);
  assert(tokens[3].type == l_identifier && strcmp(tokens[3].string_val, "_") == 0);
  assert(tokens[4].type == t_eof);

  free_tokens(tokens, count);
}

static void test_operators(void) {
  const char *src = "+ ++ += - -- -= -> * *= / /= % %= = == ! != < <= << > >= >> & && | || ^ ~ @ . ,";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  token_type_t expected[] = {
    s_plus, s_plus_plus, s_plus_equals,
    s_minus, s_minus_minus, s_minus_equals, s_arrow,
    s_star, s_star_equals,
    s_divide, s_divide_equals,
    s_modulus, s_modulus_equals,
    s_equals, s_equals_equals,
    s_bang, s_bang_equals,
    s_lt, s_lte, s_l_shift,
    s_gt, s_gte, s_r_shift,
    s_ampersand, s_and,
    s_pipe, s_or,
    s_caret, s_not, s_mem_lookup, s_dot, s_comma,
    t_eof
  };
  unsigned num_expected = sizeof(expected) / sizeof(expected[0]);
  assert(count == num_expected);
  for (unsigned i = 0; i < num_expected; i++) {
    assert(tokens[i].type == expected[i]);
  }

  free_tokens(tokens, count);
}

static void test_comments(void) {
  const char *src = "foo // this is a comment\nbar";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].type == l_identifier && strcmp(tokens[0].string_val, "foo") == 0);
  assert(tokens[1].type == l_identifier && strcmp(tokens[1].string_val, "bar") == 0);
  assert(tokens[1].loc.line == 2);
  assert(tokens[2].type == t_eof);

  free_tokens(tokens, count);
}

static void test_block_comments(void) {
  const char *src = "foo /* block\ncomment */ bar";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].type == l_identifier && strcmp(tokens[0].string_val, "foo") == 0);
  assert(tokens[1].type == l_identifier && strcmp(tokens[1].string_val, "bar") == 0);
  assert(tokens[1].loc.line == 2);
  assert(tokens[2].type == t_eof);

  free_tokens(tokens, count);
}

static void test_locations(void) {
  const char *src = "u8 x = 10;";
  unsigned count = 0;
  token_t *tokens = tokenize("test.c02", src, (long)strlen(src), &count);
  assert(tokens != NULL);

  assert(tokens[0].loc.line == 1 && tokens[0].loc.column == 1);
  assert(strcmp(tokens[0].loc.file_path, "test.c02") == 0);
  assert(tokens[1].loc.column == 4);

  free_tokens(tokens, count);
}

static void test_bad_input_returns_null(void) {
  const char *src = "u8 x = $;";
  unsigned count = 0;
  token_t *tokens = tokenize("test", src, (long)strlen(src), &count);
  assert(tokens == NULL);
}

int main(void) {
  test_keywords();
  test_number_formats();
  test_special_literals();
  test_strings();
  test_identifiers();
  test_operators();
  test_comments();
  test_block_comments();
  test_locations();
  test_bad_input_returns_null();

  printf("all tokenizer smoke tests passed\n");
  return 0;
}
