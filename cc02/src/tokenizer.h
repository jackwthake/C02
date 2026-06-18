#ifndef __TOKENIZER_H__
#define __TOKENIZER_H__

#define TOKEN_TYPES                            \
  /* token_t          printable     has_val */ \
  X(t_invalid,        "INVALID"    , 0)        \
  X(Kw_fn,            "fn"         , 0)        \
  X(Kw_reg,           "reg"        , 0)        \
  X(Kw_return,        "return"     , 0)        \
  X(Kw_void,          "void"       , 0)        \
  X(Kw_if,            "if"         , 0)        \
  X(Kw_else,          "else"       , 0)        \
  X(Kw_while,         "while"      , 0)        \
  X(Kw_for,           "for"        , 0)        \
  X(t_u8,             "u8"         , 0)        \
  X(t_i8,             "i8"         , 0)        \
  X(t_u16,            "u16"        , 0)        \
  X(t_i16,            "i16"        , 0)        \
  X(s_mem_lookup,     "@"          , 0)        \
  X(s_arrow,          "->"         , 0)        \
  X(s_divide,         "/"          , 0)        \
  X(s_divide_equals,  "/="         , 0)        \
  X(s_lparen,         "("          , 0)        \
  X(s_rparen,         ")"          , 0)        \
  X(s_lbrace,         "{"          , 0)        \
  X(s_rbrace,         "}"          , 0)        \
  X(s_semicolon,      ";"          , 0)        \
  X(s_ampersand,      "&"          , 0)        \
  X(s_comma,          ","          , 0)        \
  X(s_modulus,        "%"          , 0)        \
  X(s_modulus_equals, "%="         , 0)        \
  X(s_plus,           "+"          , 0)        \
  X(s_plus_plus,      "++"         , 0)        \
  X(s_minus,          "-"          , 0)        \
  X(s_minus_minus,    "--"         , 0)        \
  X(s_equals,         "="          , 0)        \
  X(s_star,           "*"          , 0)        \
  X(s_star_equals,    "*="         , 0)        \
  X(s_equals_equals,  "=="         , 0)        \
  X(s_plus_equals,    "+="         , 0)        \
  X(s_minus_equals,   "-="         , 0)        \
  X(s_bang,           "!"          , 0)        \
  X(s_bang_equals,    "!="         , 0)        \
  X(s_lt,             "<"          , 0)        \
  X(s_gt,             ">"          , 0)        \
  X(s_lte,            "<="         , 0)        \
  X(s_gte,            ">="         , 0)        \
  X(s_and,            "&&"         , 0)        \
  X(s_or,             "||"         , 0)        \
  X(l_num,            "NUMBER"     , 1)        \
  X(l_string,         "STRING"     , 1)        \
  X(l_identifier,     "IDENTIFIER" , 1)        \
  X(t_eof,            "END_OF_FILE", 0)

typedef enum {
  #define X(tok, str, has_val) tok,
  TOKEN_TYPES
  #undef X
} token_type_t;

typedef struct {
  token_type_t type;
  unsigned line, column, length;
  char *file_path;
  void *value;
} token_t;

const char *token_type_to_string(token_type_t type);
unsigned token_has_value(token_type_t type);
char *token_val_to_string(const token_t tok, unsigned *should_free);

void print_error_line(unsigned line, unsigned column, unsigned length);
void print_tokens(const token_t *tokens, unsigned count);

token_t *tokenize(const char *file_path, const char *source_code, const long file_size, unsigned *num_tokens);
void free_tokens(token_t *tokens, unsigned count);

#endif