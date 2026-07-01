#include "tokenizer.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "colors.h"

/**
 * MATCH_KEYWORD(kw, tok)
 * Checks if the current position in the source code matches the given keyword `kw`.
 */
#define MATCH_KEYWORD(kw, tok)                                                 \
  if (strncmp(*ptr, kw, sizeof(kw) - 1) == 0                                   \
      && !isalnum((unsigned char)(*ptr)[sizeof(kw) - 1])                       \
      && (*ptr)[sizeof(kw) - 1] != '_') {                                      \
    add_token(tokens, token_count, tok, line, *column, sizeof(kw) - 1,         \
              file_path);                                                      \
    *ptr += sizeof(kw) - 1;                                                    \
    *column += sizeof(kw) - 1;                                                 \
    return 1;                                                                  \
  }

#define MATCH_KEYWORD_NUM_VAL(kw, tok, val)                                    \
  if (strncmp(*ptr, kw, sizeof(kw) - 1) == 0                                   \
      && !isalnum((unsigned char)(*ptr)[sizeof(kw) - 1])                       \
      && (*ptr)[sizeof(kw) - 1] != '_') {                                      \
    token_t *new_tok = add_token(tokens, token_count, tok, line, *column,      \
                                 sizeof(kw) - 1, file_path);                   \
    new_tok->num_val = val;                                                    \
    *ptr += sizeof(kw) - 1;                                                    \
    *column += sizeof(kw) - 1;                                                 \
    return 1;                                                                  \
  }

/**
 * DOUBLE_OR_SINGLE_CHAR_TOKEN_(peek_ch, tok2, tok1)
 * Helper macro to handle tokens that can be either a single character (tok1) or a
 * double character (tok2) depending on the next character in the source code (peek_ch).
 */
#define DOUBLE_OR_SINGLE_CHAR_TOKEN_(peek_ch, tok2, tok1)                      \
    if (*(*ptr + 1) == (peek_ch)) {                                            \
      add_token(tokens, token_count, tok2, line, *column, 2,                   \
                file_path);                                                    \
      *ptr += 2; (*column) += 2;                                               \
    } else {                                                                   \
      add_token(tokens, token_count, tok1, line, *column, 1,                   \
                file_path);                                                    \
      (*ptr)++; (*column)++;                                                   \
    }                                                                          \
    return 1;

/**
 * MATCH_DOUBLE_OR_SINGLE_TOKEN(ch, peek_ch, tok2, tok1)
 * Macro to match a token that can be either a single character (tok1) or a
 * double character (tok2) based on the next character in the source code (peek_ch).
 */
#define MATCH_DOUBLE_OR_SINGLE_TOKEN(ch, peek_ch, tok2, tok1)                  \
  case ch: {                                                                   \
    if (*(*ptr + 1) == (peek_ch)) {                                            \
      add_token(tokens, token_count, tok2, line, *column, 2, file_path);       \
      *ptr += 2; (*column) += 2;                                               \
    } else if ((tok1) != t_invalid) {                                          \
      add_token(tokens, token_count, tok1, line, *column, 1, file_path);       \
      (*ptr)++; (*column)++;                                                   \
    } else {                                                                   \
      return 0;                                                                \
    }                                                                          \
    return 1;                                                                  \
  }

#define TRIPPLE_TOKEN_CHAR_(ch, DOUBLE_TYPE, EQ_TYPE, SINGLE_TYPE)                \
  do {                                                                            \
    if (*(*ptr + 1) == (ch)) {                                                    \
      add_token(tokens, token_count, (DOUBLE_TYPE), line, *column, 2, file_path); \
      *ptr += 2; (*column) += 2;                                                  \
      return 1;                                                                   \
    }                                                                             \
    DOUBLE_OR_SINGLE_CHAR_TOKEN_('=', (EQ_TYPE), (SINGLE_TYPE))                   \
  } while (0)


#define TRIPPLE_TOKEN_CHAR(ch, DOUBLE_TYPE, EQ_TYPE, SINGLE_TYPE) \
  case ch: {                                                      \
    TRIPPLE_TOKEN_CHAR_(ch, DOUBLE_TYPE, EQ_TYPE, SINGLE_TYPE);   \
  }


#define PRINT_ERROR_HEADER(file_path, err_line, err_col)                                                                   \
  fprintf(stderr, BOLD_WHITE "%s" RESET ":" BOLD_WHITE "%u" RESET ":" BOLD_WHITE "%u" RESET ": " BOLD_RED "error: " RESET, \
    file_path, err_line, err_col)

static token_t* add_token(token_t *tokens, unsigned *token_count, token_type_t type, unsigned line, unsigned column, unsigned length, char *file_path) {
  token_t *tok = &tokens[(*token_count)++];
  tok->type = type;
  tok->loc.line = line;
  tok->loc.column = column;
  tok->loc.length = length;
  tok->loc.file_path = file_path;
  tok->num_val = 0; // zero out the union just to be safe
  return tok;
}

/**
 * MATCH_SINGLE_CHAR_SYMBOL(ch, tok)
 * Macro to match a single character symbol (ch) and add the corresponding token (tok).
 */
#define MATCH_SINGLE_CHAR_SYMBOL(ch, tok)                                   \
  case ch:                                                                  \
    add_token(tokens, token_count, tok, line, *column, 1, file_path);       \
    (*ptr)++; (*column)++;                                                  \
    return 1;


const char *token_type_to_string(token_type_t type) {
  switch (type) {
    #define X(tok, str, has_val) case tok: return str;
    TOKEN_TYPES
    #undef X
    default: return "UNKNOWN";
  }
}  


unsigned token_has_value(token_type_t type) {
  switch (type) {
    #define X(tok, str, has_val) case tok: return has_val;
    TOKEN_TYPES
    #undef X
    default: return 0;
  }
}

char *token_val_to_string(const token_t tok, unsigned *should_free) {
  if (!token_has_value(tok.type)) {
    *should_free = 0;
    return NULL;
  }

  if (tok.type == l_identifier || tok.type == l_string) {
    *should_free = 0;
    return (char *)tok.string_val;
  }

  if (tok.type == l_num) {
    char *str = malloc(32);
    if (str == NULL) {
      return NULL;
    }

    snprintf(str, 32, "%ld", tok.num_val);
    *should_free = 1;
    return str;
  }

  return NULL;
}


void print_tokens(const token_t *tokens, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const token_t token = tokens[i];

    if (token.type == l_identifier || token.type == l_string) {
      printf("Token: %s(%s), %s:%u:%u\n", token_type_to_string(token.type), (char *)token.string_val, token.loc.file_path, token.loc.line, token.loc.column);
    } else if (token.type == l_num) {
      printf("Token: %s(%ld), %s:%u:%u\n", token_type_to_string(token.type), token.num_val, token.loc.file_path, token.loc.line, token.loc.column);
    } else {
      printf("Token: %s, %s:%u:%u\n", token_type_to_string(token.type), token.loc.file_path, token.loc.line, token.loc.column);
    }

    if (token.type == t_eof) {
      break;
    }
  }
}


void free_tokens(token_t *tokens, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (tokens[i].type == l_identifier || tokens[i].type == l_string) {
      free(tokens[i].string_val);
    }
  }
  free(tokens);
}


/**
 * skip_whitespace_and_comments(ptr, line, column)
 * Skips over whitespace and comments in the source code, updating the line and column numbers accordingly.
 * Returns 1 if there is more source code to process after skipping, or 0 if the end of the source code is reached.
 */
static int skip_whitespace_and_comments(char **ptr, unsigned *line, unsigned *column) {
  while (**ptr != '\0') {
    // skip whitespace
    if (isspace((unsigned char)**ptr)) {
      if (**ptr == '\n') {
        (*line)++;
        *column = 1;
      } else {
        (*column)++;
      }
      (*ptr)++;
      continue;
    }

    // skip comments
    if (**ptr == '/' && *(*ptr + 1) == '/') {
      *ptr += 2;
      *column += 2;
      while (**ptr != '\0' && **ptr != '\n') {
        (*ptr)++;
        (*column)++;
      }
      continue;
    }

    // skip block comments
    if (**ptr == '/' && *(*ptr + 1) == '*') {
      *ptr += 2;
      *column += 2;
      while (**ptr != '\0' && !(**ptr == '*' && *(*ptr + 1) == '/')) {
        if (**ptr == '\n') {
          (*line)++;
          *column = 1;
        } else {
          (*column)++;
        }
        (*ptr)++;
      }
      if (**ptr != '\0') {
        *ptr += 2;
        *column += 2;
      }
      continue;
    }

    break; // Not whitespace or comment
  }

  return **ptr != '\0'; // Return whether we have more to process
}


static int tokenize_symbol(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
  char c = **ptr;
  switch (c) {
    MATCH_SINGLE_CHAR_SYMBOL(';', s_semicolon);
    MATCH_SINGLE_CHAR_SYMBOL('(', s_lparen);
    MATCH_SINGLE_CHAR_SYMBOL(')', s_rparen);
    MATCH_SINGLE_CHAR_SYMBOL('{', s_lbrace);
    MATCH_SINGLE_CHAR_SYMBOL('}', s_rbrace);
    MATCH_SINGLE_CHAR_SYMBOL('@', s_mem_lookup);
    MATCH_SINGLE_CHAR_SYMBOL(',', s_comma);
    MATCH_SINGLE_CHAR_SYMBOL('.', s_dot);
    MATCH_SINGLE_CHAR_SYMBOL('^', s_caret);
    MATCH_SINGLE_CHAR_SYMBOL('~', s_not);

    MATCH_DOUBLE_OR_SINGLE_TOKEN('%', '=', s_modulus_equals, s_modulus);
    MATCH_DOUBLE_OR_SINGLE_TOKEN('*', '=', s_star_equals, s_star);
    MATCH_DOUBLE_OR_SINGLE_TOKEN('/', '=', s_divide_equals, s_divide);
    MATCH_DOUBLE_OR_SINGLE_TOKEN('&', '&', s_and, s_ampersand);
    MATCH_DOUBLE_OR_SINGLE_TOKEN('=', '=', s_equals_equals, s_equals);
    MATCH_DOUBLE_OR_SINGLE_TOKEN('!', '=', s_bang_equals, s_bang);
    MATCH_DOUBLE_OR_SINGLE_TOKEN('|', '|', s_or, s_pipe);

    TRIPPLE_TOKEN_CHAR('+', s_plus_plus, s_plus_equals, s_plus);
    TRIPPLE_TOKEN_CHAR('<', s_l_shift, s_lte, s_lt);
    TRIPPLE_TOKEN_CHAR('>', s_r_shift, s_gte, s_gt);

    case '-': { // 4 possible cases: '-', '--', '->', '-='
      if (*(*ptr + 1) == '>') {
        add_token(tokens, token_count, s_arrow, line, *column, 2, file_path);
        *ptr += 2; (*column) += 2;
        return 1;
      } else if (*(*ptr + 1) == '-') {
        add_token(tokens, token_count, s_minus_minus, line, *column, 2, file_path);
        *ptr += 2; (*column) += 2;
        return 1;
      }
      DOUBLE_OR_SINGLE_CHAR_TOKEN_('=', s_minus_equals, s_minus)
    }

    default:
      return 0; // Not a recognized symbol
  }
}


// used solely for error printing in below functions, set at the beggining of the tokenize() func
// meaning print_error_line functions will fail if called before tokenizing... why would anyone do that tho?
static char *source;


/**
 * get_line_number_from_ptr(ptr)
 * Helper function to get the line number that a pointer in the source code is pointing to.
 * If the pointer is outside the source buffer, returns the last line number.
 */
static unsigned get_line_number_from_ptr(const char *ptr) {
  if (!ptr || !source) return 1;

  unsigned current_line = 1;
  const char *current_ptr = source;

  while (*current_ptr != '\0' && current_ptr < ptr) {
    if (*current_ptr == '\n') {
      current_line++;
    }
    current_ptr++;
  }

  return current_line;
}


/**
 * print_error_line_(source, ptr, column, error_length)
 * Helper function to print the line of source code where an error occurred, along with a caret
 * pointing to the column of the error.
 */
static void print_error_line_(const char *ptr, unsigned line_number, unsigned column, unsigned length) {
  if (!ptr) return;
  
  // walk back to start of line
  const char *line_start = ptr;
  while (line_start > source && *(line_start - 1) != '\n') {
      line_start--;
  }

  // walk forward to end of line
  const char *line_end = ptr;
  while (*line_end != '\0' && *line_end != '\n') {
      line_end++;
  }

  if (length == 0) length = 1;

  // print the line
  fprintf(stderr, BOLD_BLUE "%u |" RESET " %.*s\n", line_number, (int)(line_end - line_start), line_start);

  // print the caret + span
  fprintf(stderr, BOLD_BLUE "%*s |" BOLD_RED " %*s", (int)( // align the pipe with the line number digits
    (line_number >= 1000) ? 4 :
    (line_number >= 100)  ? 3 :
    (line_number >= 10)   ? 2 : 1
  ), "", (int)(column - 1), "");
  for (unsigned i = 0; i < length; i++) {
    fputc(i == 0 ? '^' : '~', stderr);
  }
  fprintf(stderr, RESET "\n");
}


/**
 * print_error_line(loc)
 * Public facing helper function to print the line of source code where an error occurred, 
 * along with a caret pointing to the column of the error.
 * Used in parser, semantic analysis error reporting
*/
void print_error_line(token_location_t loc) {
  if (!source) return;

  const char *ptr = source;
  unsigned current_line = 1;

  while (*ptr != '\0' && current_line < loc.line) {
    if (*ptr == '\n') {
      current_line++;
      if (current_line == loc.line) {
        ptr++;
        break;
      }
    }
    ptr++;
  }

  if (*ptr == '\0' && current_line < loc.line) {
    // If the requested line number is past the end, fall back to the last line.
    ptr = source;
    while (*ptr != '\0' && *ptr != '\n') {
      ptr++;
    }
    if (*ptr == '\n') {
      ptr++;
    }
  }

  print_error_line_(ptr, loc.line, loc.column, loc.length);
}


static int tokenize_keyword_or_identifier(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
  MATCH_KEYWORD("fn",       Kw_fn)
  MATCH_KEYWORD("decl",     Kw_fwd_decl)
  MATCH_KEYWORD("reg",      Kw_reg)
  MATCH_KEYWORD("return",   Kw_return)
  MATCH_KEYWORD("struct",   Kw_struct)
  MATCH_KEYWORD("break",    Kw_break)
  MATCH_KEYWORD("continue", Kw_continue)
  MATCH_KEYWORD("void",     Kw_void)
  MATCH_KEYWORD("if",       Kw_if)
  MATCH_KEYWORD("else",     Kw_else)
  MATCH_KEYWORD("while",    Kw_while)
  MATCH_KEYWORD("for",      Kw_for)
  MATCH_KEYWORD("u8",       t_u8)
  MATCH_KEYWORD("i8",       t_i8)
  MATCH_KEYWORD("u16",      t_u16)
  MATCH_KEYWORD("i16",      t_i16)

  MATCH_KEYWORD_NUM_VAL("null", l_num, 0) // treat 'null' as a special numeric literal with value 0
  MATCH_KEYWORD_NUM_VAL("false", l_num, 0) // treat 'false' as a special numeric literal with value 0
  MATCH_KEYWORD_NUM_VAL("true", l_num, 1) // treat 'true' as a special numeric literal with value 1

  if (isalpha((unsigned char)**ptr) || **ptr == '_') {
    // Handle identifiers (and potentially keywords that aren't reserved)
    const char *start = *ptr;
    while (isalnum((unsigned char)**ptr) || **ptr == '_') {
      (*ptr)++;
      (*column)++;
    }
   
    size_t length = (size_t)(*ptr - start);
    char *identifier = strndup(start, length);
    if (!identifier) {
      perror("Failed to allocate memory for identifier");
      return -1;
    }
   
    token_t *tok = add_token(tokens, token_count, l_identifier, line, *column - (unsigned)length, (unsigned)length, file_path);
    tok->string_val = identifier;
    return 1;
  }

  return 0;
}


static int tokenize_string(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
  if (**ptr != '"') {
    return 0; // Not a string literal
  }

  const char *string_start = *ptr;
  const unsigned string_column = *column;

  (*ptr)++; // Skip opening quote
  (*column)++;

  // First scan to the closing quote, advancing line/column. A backslash
  // escapes the next character, so an escaped quote (\") doesn't end the
  // string.
  const char *content_start = *ptr;
  while (**ptr != '\0' && **ptr != '"' && **ptr != '\n') {
    if (**ptr == '\\') {
      (*ptr)++; // skip the backslash; the escaped char is consumed below
      (*column)++;
      if (**ptr == '\0') {
        break;
      }
    }
    (*ptr)++;
    (*column)++;
  }

  if (**ptr != '"') {
    unsigned err_line = get_line_number_from_ptr(string_start);
    PRINT_ERROR_HEADER(file_path, err_line, string_column);
    fprintf(stderr, "Unterminated string literal\n");
    print_error_line((token_location_t){ .line = err_line, .column = string_column, .length = 1, .file_path = file_path });
    return -1;
  }

  // Decode the raw [content_start, *ptr) span into a fresh buffer, translating
  // escape sequences. The decoded form is never longer than the raw span (each
  // escape collapses two source bytes into one), so raw_span + 1 always fits -
  // copying the raw span directly (the old strndup of a logical length that
  // undercounted escapes) dropped one trailing byte per escape.
  size_t raw_span = (size_t)(*ptr - content_start);
  char *string_literal = malloc(raw_span + 1);
  if (!string_literal) {
    perror("Failed to allocate memory for string literal");
    return -1;
  }

  size_t out_len = 0;
  for (const char *s = content_start; s < *ptr; ++s) {
    if (*s == '\\' && (s + 1) < *ptr) {
      ++s; // consume the escaped character
      switch (*s) {
        case 'n':  string_literal[out_len++] = '\n'; break;
        case 't':  string_literal[out_len++] = '\t'; break;
        case 'r':  string_literal[out_len++] = '\r'; break;
        case '0':  string_literal[out_len++] = '\0'; break;
        case '\\': string_literal[out_len++] = '\\'; break;
        case '"':  string_literal[out_len++] = '"';  break;
        case '\'': string_literal[out_len++] = '\''; break;
        default:   string_literal[out_len++] = *s;   break; // unknown escape: keep the char verbatim
      }
    } else {
      string_literal[out_len++] = *s;
    }
  }
  string_literal[out_len] = '\0';

  (*ptr)++; // Skip closing quote
  (*column)++;

  // loc spans the whole literal including both quotes, anchored at the opening
  // quote - derived from source offsets so it stays correct no matter how many
  // escapes shortened the decoded string.
  const unsigned literal_length = (unsigned)(*ptr - string_start);
  token_t *tok = add_token(tokens, token_count, l_string, line, string_column, literal_length, file_path);
  tok->string_val = string_literal;
  return 1;
}


/**
 * tokenize_number(tokens, token_count, ptr, line, column, file_path)
 * Attempts to tokenize a number literal starting at the current position in the source code.
 * Supports decimal, hexadecimal (0x), and binary (0b) literals.
 * If a valid number literal is found, it is added to the tokens array and the function returns 1.
 * If the current position does not start a valid number literal, the function returns 0.
 * If an error occurs while tokenizing the number (e.g., invalid format), the function returns -1 and prints an error message.
 */
static int tokenize_number(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
  const char *literal_start = *ptr;
  int base = 10;

  if (literal_start[0] == '0' && (literal_start[1] == 'x' || literal_start[1] == 'X')) {
    base = 16;
    literal_start += 2;
  } else if (literal_start[0] == '0' && (literal_start[1] == 'b' || literal_start[1] == 'B')) {
    base = 2;
    literal_start += 2;
  } else if (!isdigit((unsigned char)literal_start[0])) {
    return 0; // Not a number literal
  }

  const char *end = literal_start;
  if (base == 16) {
    while (isxdigit((unsigned char)*end)) {
      end++;
    }
  } else if (base == 2) {
    while (*end == '0' || *end == '1') {
      end++;
    }
  } else {
    while (isdigit((unsigned char)*end)) {
      end++;
    }
  }

  if (end == literal_start) {
    // no valid digits (e.g. "0x" with nothing after) - consume the prefix so
    // tokenizing makes forward progress instead of re-reporting the same spot
    unsigned prefix_len = (base != 10) ? 2 : 1;
    PRINT_ERROR_HEADER(file_path, line, *column);
    fprintf(stderr, "Invalid number literal\n");
    print_error_line((token_location_t){ .line = line, .column = *column, .length = prefix_len, .file_path = file_path });
    *ptr += prefix_len;
    *column += prefix_len;
    return -1;
  }

  const unsigned literal_length = (unsigned)(end - *ptr);

  char *endptr = NULL;
  errno = 0;
  long value = strtol(literal_start, &endptr, base);
  if (endptr != (char *)end || errno == ERANGE) {
    PRINT_ERROR_HEADER(file_path, line, *column);
    fprintf(stderr, errno == ERANGE ? "Integer literal is too large to represent\n"
                                    : "Invalid number literal\n");
    print_error_line((token_location_t){ .line = line, .column = *column, .length = literal_length, .file_path = file_path });
    // consume the whole malformed literal so it's reported exactly once, not
    // re-scanned digit by digit
    *ptr = (char *)end;
    *column += literal_length;
    return -1;
  }

  token_t *tok = add_token(tokens, token_count, l_num, line, *column, literal_length, file_path);
  tok->num_val = value;

  *ptr = (char *)end;
  *column += literal_length;
  return 1;
}


token_t *tokenize(const char *file_path, const char *source_code, const long file_size, unsigned *num_tokens) {
  const unsigned max_tokens = (unsigned)(file_size); // lazy upper bound on number of tokens, can't be more than 1 token per character
  char *ptr = source = (char *)source_code;
  unsigned line = 1, column = 1, token_count = 0, error_count = 0;

  token_t *tokens = calloc(max_tokens, sizeof(*tokens));
  if (!tokens) {
    perror("Failed to allocate memory for tokens");
    return NULL;
  }

  while (*ptr != '\0' && token_count < max_tokens) {
    // skip whitespace
    if (!skip_whitespace_and_comments(&ptr, &line, &column)) {
      break; // End of source code
    }

    if (tokenize_symbol(tokens, &token_count, &ptr, line, &column, (char *)file_path)) {
      continue;
    }

    int is_string_result = tokenize_string(tokens, &token_count, &ptr, line, &column, (char *)file_path);
    if (is_string_result < 0) {
      error_count++;
      if (*ptr == '\n') { line++; column = 1; } else { column++; }
      ptr++;
      continue;
    } else if (is_string_result > 0) {
      continue; // Successfully tokenized a string
    }

    int is_number_result = tokenize_number(tokens, &token_count, &ptr, line, &column, (char *)file_path);
    if (is_number_result < 0) {
      error_count++;
      continue; // tokenize_number consumed the offending characters itself
    } else if (is_number_result > 0) {
      continue;
    }

    int is_keyword_or_id_result = tokenize_keyword_or_identifier(tokens, &token_count, &ptr, line, &column, (char *)file_path);
    if (is_keyword_or_id_result < 0) {
      error_count++;
      if (*ptr == '\n') { line++; column = 1; } else { column++; }
      ptr++;
      continue;
    } else if (is_keyword_or_id_result > 0) {
      continue;
    }

    // If we reach here, it's an unrecognized token.
    PRINT_ERROR_HEADER(file_path, line, column);
    fprintf(stderr, "Unexpected character '%c'\n", *ptr);
    print_error_line((token_location_t){ .line = line, .column = column, .length = 1, .file_path = (char *)file_path });
    error_count++;

    if (*ptr == '\n') { line++; column = 1; } else { column++; }
    ptr++;
  }

  if (error_count > 0) {
    fprintf(stderr, RED "Tokenization failed with " BOLD_RED "%u" RESET RED " error(s)\n" RESET, error_count);
    free_tokens(tokens, token_count);
    return NULL;
  }

  if (token_count >= max_tokens) {
    fprintf(stderr, BOLD_RED "error:" RESET " token array overflow while tokenizing " BOLD_WHITE "%s\n" RESET, file_path);
    free_tokens(tokens, token_count);
    return NULL;
  }

  add_token(tokens, &token_count, t_eof, line, column, 0, (char *)file_path);

  *num_tokens = token_count;
  return tokens;
}