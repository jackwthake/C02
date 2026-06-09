#ifndef __TOKENIZER_H__
#define __TOKENIZER_H__

typedef enum {
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
  s_star,           // *, pointer and multiply
  s_arrow,          // ->
  s_plus,           // +
  s_minus,          // -
  s_divide,         // /
  s_lparen,         // (
  s_rparen,         // )
  s_lbrace,         // {
  s_rbrace,         // }
  s_semicolon,      // ;
  s_equals,         // =
  s_equalsequals,   // ==
  s_plus_equals,    // +=
  s_minus_equals,   // -=
  s_bang,           // !
  s_bang_equals,     // !=
  s_lt,             // <
  s_gt,             // >
  s_lte,            // <=
  s_gte,            // >=
  s_ampersand,      // & (Address of operator)
  s_comma,          // , (for function args)
  
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

token_t *tokenize(const char *file_path, const char *source_code);

#endif