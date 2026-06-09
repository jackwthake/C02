#ifndef __TOKENIZER_H__
#define __TOKENIZER_H__

typedef enum {
  t_invalid = 0,

  // KEYWORDS
  Kw_fn,
  Kw_reg,
  Kw_return,
  Kw_void,
  Kw_if,
  Kw_else,
  Kw_while,
  Kw_for,

  // TYPES
  t_u8,             // only integer based types, chars can be u8, 6502 has no FPU
  t_i8,
  t_u16,
  t_i16,

  // SYMBOLS
  s_mem_lookup,     // @
  s_arrow,          // ->
  s_divide,         // /
  s_lparen,         // (
  s_rparen,         // )
  s_lbrace,         // {
  s_rbrace,         // }
  s_semicolon,      // ;
  s_ampersand,      // & (Address of operator)
  s_comma,          // , (for function args)
  s_plus,           // +
  s_minus,          // -
  s_equals,         // =
  s_star,           // *, pointer and multiply
  s_equalsequals,   // ==
  s_plus_equals,    // +=
  s_minus_equals,   // -=
  s_bang,           // !
  s_bang_equals,    // !=
  s_lt,             // <
  s_gt,             // >
  s_lte,            // <=
  s_gte,            // >=
  
  // LITERALS
  l_num,
  l_string,
  l_identifier,

  // SPECIAL
  t_eof,
} token_type_t;

typedef struct {
  token_type_t type;
  unsigned line, column;
  char *file_path;
  void *value;
} token_t;

const char *token_type_to_string(token_type_t type);
void print_tokens(const token_t *tokens, unsigned count);

token_t *tokenize(const char *file_path, const char *source_code, const long file_size, unsigned *num_tokens);
void free_tokens(token_t *tokens, unsigned count);

#endif