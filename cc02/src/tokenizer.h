#ifndef __TOKENIZER_H__
#define __TOKENIZER_H__

#define TOKEN_TYPES                 \
  X(t_invalid,       "INVALID")     \
  X(Kw_fn,           "fn")          \
  X(Kw_reg,          "reg")         \
  X(Kw_return,       "return")      \
  X(Kw_void,         "void")        \
  X(Kw_if,           "if")          \
  X(Kw_else,         "else")        \
  X(Kw_while,        "while")       \
  X(Kw_for,          "for")         \
  X(t_u8,            "u8")          \
  X(t_i8,            "i8")          \
  X(t_u16,           "u16")         \
  X(t_i16,           "i16")         \
  X(s_mem_lookup,    "@")           \
  X(s_arrow,         "->")          \
  X(s_divide,        "/")           \
  X(s_divide_equals, "/=")          \
  X(s_lparen,        "(")           \
  X(s_rparen,        ")")           \
  X(s_lbrace,        "{")           \
  X(s_rbrace,        "}")           \
  X(s_semicolon,     ";")           \
  X(s_ampersand,     "&")           \
  X(s_comma,         ",")           \
  X(s_plus,          "+")           \
  X(s_minus,         "-")           \
  X(s_equals,        "=")           \
  X(s_star,          "*")           \
  X(s_star_equals,   "*=")          \
  X(s_equalsequals,  "==")          \
  X(s_plus_equals,   "+=")          \
  X(s_minus_equals,  "-=")          \
  X(s_bang,          "!")           \
  X(s_bang_equals,   "!=")          \
  X(s_lt,            "<")           \
  X(s_gt,            ">")           \
  X(s_lte,           "<=")          \
  X(s_gte,           ">=")          \
  X(s_and,           "&&")          \
  X(s_or,            "||")          \
  X(l_num,           "NUMBER")      \
  X(l_string,        "STRING")      \
  X(l_identifier,    "IDENTIFIER")  \
  X(t_eof,           "END_OF_FILE")

typedef enum {
  #define X(tok, str) tok,
  TOKEN_TYPES
  #undef X
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