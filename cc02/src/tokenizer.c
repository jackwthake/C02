#include "tokenizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/**
 * MATCH_KEYWORD(kw, tok)
 * Checks if the current position in the source code matches the given keyword `kw`.
 */
#define MATCH_KEYWORD(kw, tok, val)                                         \
  if (strncmp(*ptr, kw, sizeof(kw) - 1) == 0                                \
      && !isalnum((unsigned char)(*ptr)[sizeof(kw) - 1])                    \
      && (*ptr)[sizeof(kw) - 1] != '_') {                                   \
    add_token(tokens, token_count, tok, line, *column, file_path, val);     \
    *ptr += sizeof(kw) - 1;                                                 \
    *column += sizeof(kw) - 1;                                              \
    return 1;                                                               \
  }

/**
 * DOUBLE_OR_SINGLE_CHAR_TOKEN_(peek_ch, tok2, tok1)
 * Helper macro to handle tokens that can be either a single character (tok1) or a
 * double character (tok2) depending on the next character in the source code (peek_ch).
 */
#define DOUBLE_OR_SINGLE_CHAR_TOKEN_(peek_ch, tok2, tok1)                   \
    if (*(*ptr + 1) == (peek_ch)) {                                         \
      add_token(tokens, token_count, tok2, line, *column,                   \
                file_path, NULL);                                           \
      *ptr += 2; (*column) += 2;                                            \
    } else {                                                                \
      add_token(tokens, token_count, tok1, line, *column,                   \
                file_path, NULL);                                           \
      (*ptr)++; (*column)++;                                                \
    }                                                                       \
    return 1;

/**
 * MATCH_DOUBLE_OR_SINGLE_TOKEN(ch, peek_ch, tok2, tok1)
 * Macro to match a token that can be either a single character (tok1) or a
 * double character (tok2) based on the next character in the source code (peek_ch).
 */
#define MATCH_DOUBLE_OR_SINGLE_TOKEN(ch, peek_ch, tok2, tok1)               \
  case ch: {                                                                \
    if (*(*ptr + 1) == (peek_ch)) {                                         \
      add_token(tokens, token_count, tok2, line, *column, file_path, NULL); \
      *ptr += 2; (*column) += 2;                                            \
    } else if ((tok1) != t_invalid) {                                       \
      add_token(tokens, token_count, tok1, line, *column, file_path, NULL); \
      (*ptr)++; (*column)++;                                                \
    } else {                                                                \
      return 0;                                                             \
    }                                                                       \
    return 1;                                                               \
  }


static void add_token(token_t *tokens, unsigned *token_count, token_type_t type, unsigned line, unsigned column, char *file_path, void *value) {
  tokens[(*token_count)++] = (token_t){
    .type = type,
    .line = line,
    .column = column,
    .file_path = file_path,
    .value = value
  };
}

/**
 * MATCH_SINGLE_CHAR_SYMBOL(ch, tok)
 * Macro to match a single character symbol (ch) and add the corresponding token (tok).
 */
#define MATCH_SINGLE_CHAR_SYMBOL(ch, tok)                                   \
  case ch:                                                                  \
    add_token(tokens, token_count, tok, line, *column, file_path, NULL);    \
    (*ptr)++; (*column)++;                                                  \
    return 1;


const char *token_type_to_string(token_type_t type) {
  switch (type) {
    #define X(tok, str) case tok: return str;
    TOKEN_TYPES
    #undef X
    default: return "UNKNOWN";
  }
}  


void print_tokens(const token_t *tokens, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const token_t token = tokens[i];

    if (token.type == l_identifier || token.type == l_string) {
      printf("Token: %s(%s), %s:%u:%u\n", token_type_to_string(token.type), (char *)token.value, token.file_path, token.line, token.column);
    } else if (token.type == l_num) {
      printf("Token: %s(%ld), %s:%u:%u\n", token_type_to_string(token.type), *(long *)token.value, token.file_path, token.line, token.column);
    } else {
      printf("Token: %s, %s:%u:%u\n", token_type_to_string(token.type), token.file_path, token.line, token.column);
    }

    if (token.type == t_eof) {
      break;
    }
  }
}


void free_tokens(token_t *tokens, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (tokens[i].type == l_identifier || tokens[i].type == l_string || tokens[i].type == l_num) {
      free(tokens[i].value);
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
    MATCH_SINGLE_CHAR_SYMBOL(';', s_semicolon)
    MATCH_SINGLE_CHAR_SYMBOL('(', s_lparen)
    MATCH_SINGLE_CHAR_SYMBOL(')', s_rparen)
    MATCH_SINGLE_CHAR_SYMBOL('{', s_lbrace)
    MATCH_SINGLE_CHAR_SYMBOL('}', s_rbrace)
    MATCH_SINGLE_CHAR_SYMBOL('@', s_mem_lookup)
    MATCH_SINGLE_CHAR_SYMBOL(',', s_comma)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('*', '=', s_star_equals, s_star)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('+', '=', s_plus_equals, s_plus)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('/', '=', s_divide_equals, s_divide)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('&', '&', s_and, s_ampersand)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('=', '=', s_equalsequals, s_equals)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('!', '=', s_bang_equals, s_bang)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('<', '=', s_lte, s_lt)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('>', '=', s_gte, s_gt)
    MATCH_DOUBLE_OR_SINGLE_TOKEN('|', '|', s_or, 0) // '|' is not a valid single-character token in this language, only '||'
    case '-': // 3 possible cases: '-', '->', '-='
      if (*(*ptr + 1) == '>') {
        add_token(tokens, token_count, s_arrow, line, *column, file_path, NULL);
        *ptr += 2; (*column) += 2;
        return 1;
      }
      DOUBLE_OR_SINGLE_CHAR_TOKEN_('=', s_minus_equals, s_minus)
    default:
      return 0; // Not a recognized symbol
  }
}


/**
 * print_error_line(source, ptr, column)
 * Helper function to print the line of source code where an error occurred, along with a caret
 * pointing to the column of the error.
 */
static void print_error_line(const char *source, const char *ptr, unsigned column) {
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

  // print the line
  fprintf(stderr, "  %.*s\n", (int)(line_end - line_start), line_start);

  // print the caret
  fprintf(stderr, "  %*s^\n", (int)(column - 1), "");
}


static inline long *make_long(long value) {
  long *ptr = malloc(sizeof(*ptr));
  if (ptr) *ptr = value;
  else {
    perror("Failed to allocate memory for long value");
    exit(EXIT_FAILURE);
  }
  return ptr;
}


static int tokenize_keyword_or_identifier(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
    MATCH_KEYWORD("fn",     Kw_fn, NULL)
    MATCH_KEYWORD("reg",    Kw_reg, NULL)
    MATCH_KEYWORD("return", Kw_return, NULL)
    MATCH_KEYWORD("void",   Kw_void, NULL)
    MATCH_KEYWORD("if",     Kw_if, NULL)
    MATCH_KEYWORD("else",   Kw_else, NULL)
    MATCH_KEYWORD("while",  Kw_while, NULL)
    MATCH_KEYWORD("for",    Kw_for, NULL)
    MATCH_KEYWORD("u8",     t_u8, NULL)
    MATCH_KEYWORD("i8",     t_i8, NULL)
    MATCH_KEYWORD("u16",    t_u16, NULL)
    MATCH_KEYWORD("i16",    t_i16, NULL)
    MATCH_KEYWORD("null",   l_num, make_long(0)) // treat 'null' as a special numeric literal with value 0

  if (isalpha((unsigned char)**ptr) || **ptr == '_') {
    // Handle identifiers (and potentially keywords that aren't reserved)
    const char *start = *ptr;
    while (isalnum((unsigned char)**ptr) || **ptr == '_') {
      (*ptr)++;
      (*column)++;
    }
   
    size_t length = *ptr - start;
    char *identifier = strndup(start, length);
    if (!identifier) {
      perror("Failed to allocate memory for identifier");
      return -1;
    }
   
    add_token(tokens, token_count, l_identifier, line, *column - length, file_path, identifier);
    return 1;
  }

  return 0;
}


static int tokenize_string(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path, const char *source_code) {
  if (**ptr != '"') {
    return 0; // Not a string literal
  }

  const char *string_start = *ptr;
  const unsigned string_column = *column;

  (*ptr)++; // Skip opening quote
  (*column)++;

  const char *start = *ptr;
  unsigned length = 0;
  while (**ptr != '\0' && **ptr != '"' && **ptr != '\n') {
    if (**ptr == '\\') {
      (*ptr)++; // Skip escape character
      (*column)++;
      if (**ptr == '\0') {
        break;
      }
    }
    (*ptr)++;
    (*column)++;
    length++;
  }

  char *string_literal = strndup(start, length);
  if (!string_literal) {
    perror("Failed to allocate memory for string literal");
    return -1;
  }

  if (**ptr != '"') {
    fprintf(stderr, "Error: Unterminated string literal at %s:%u:%u\n", file_path, line, string_column);
    print_error_line(source_code, string_start, string_column);
    free(string_literal);
    return -1;
  }

  (*ptr)++; // Skip closing quote
  (*column)++;

  add_token(tokens, token_count, l_string, line, *column - length - 2, file_path, string_literal);
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
static int tokenize_number(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path, const char *source_code) {
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

  if (end == literal_start) { // NOTE: Why is line + 1 here? Shouldn't it just be line? this outputs correct line numbers...
    fprintf(stderr, "Error: Invalid number literal at %s:%u:%u\n", file_path, line + 1, *column);
    print_error_line(source_code, *ptr, *column);
    return -1;
  }

  char *endptr = NULL;
  long value = strtol(literal_start, &endptr, base);
  if (endptr != (char *)end) { // NOTE: Why is line + 1 here? Shouldn't it just be line? this outputs correct line numbers...
    fprintf(stderr, "Error: Invalid number literal at %s:%u:%u\n", file_path, line + 1, *column);
    print_error_line(source_code, *ptr, *column);
    return -1;
  }

  long *number_value = malloc(sizeof(*number_value));
  if (!number_value) {
    perror("Failed to allocate memory for number literal");
    return -1;
  }
  *number_value = value;

  add_token(tokens, token_count, l_num, line, *column, file_path, number_value);

  const unsigned literal_length = (unsigned)(end - *ptr);
  *ptr = (char *)end;
  *column += literal_length;
  return 1;
}


token_t *tokenize(const char *file_path, const char *source_code, const long file_size, unsigned *num_tokens) {
  const unsigned max_tokens = (unsigned)(file_size); // lazy upper bound on number of tokens, can't be more than 1 token per character
  char *ptr = (char *)source_code;
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

    int is_string_result = tokenize_string(tokens, &token_count, &ptr, line, &column, (char *)file_path, source_code);
    if (is_string_result < 0) {
      error_count++;
      ptr++;
      column++;
      continue;
    } else if (is_string_result > 0) {
      continue; // Successfully tokenized a string
    }

    int is_number_result = tokenize_number(tokens, &token_count, &ptr, line, &column, (char *)file_path, source_code);
    if (is_number_result < 0) {
      error_count++;
      ptr++;
      column++;
      continue;
    } else if (is_number_result > 0) {
      continue;
    }

    int is_keyword_or_id_result = tokenize_keyword_or_identifier(tokens, &token_count, &ptr, line, &column, (char *)file_path);
    if (is_keyword_or_id_result < 0) {
      error_count++;
      ptr++;
      column++;
      continue;
    } else if (is_keyword_or_id_result > 0) {
      continue;
    }

    // If we reach here, it's an unrecognized token.
    // NOTE: Why is line + 1 here? Shouldn't it just be line? this outputs correct line numbers...
    fprintf(stderr, "Error: unexpected character '%c' at %s:%u:%u\n",
      *ptr, file_path, line + 1, column);
    print_error_line(source_code, ptr, column);
    error_count++;

    ptr++;
    column++;
  }

  if (error_count > 0) {
    fprintf(stderr, "Tokenization failed with %u error(s)\n", error_count);
    free_tokens(tokens, token_count);
    return NULL;
  }

  if (token_count >= max_tokens) {
    fprintf(stderr, "Error: token array overflow while tokenizing %s\n", file_path);
    free_tokens(tokens, token_count);
    return NULL;
  }

  add_token(tokens, &token_count, t_eof, line, column, (char *)file_path, NULL);

  *num_tokens = token_count;
  return tokens;
}
