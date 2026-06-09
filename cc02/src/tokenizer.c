#include "tokenizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void add_token(token_t *tokens, unsigned *token_count, token_type_t type, unsigned line, unsigned column, char *file_path, void *value) {
  tokens[(*token_count)++] = (token_t){
    .type = type,
    .line = line,
    .column = column,
    .file_path = file_path,
    .value = value
  };
}

const char *token_type_to_string(token_type_t type) {
  switch (type) {
    case t_invalid: return "INVALID";
    case Kw_fn: return "fn";
    case Kw_reg: return "reg";
    case Kw_return: return "return";
    case Kw_void: return "void";
    case Kw_if: return "if";
    case Kw_else: return "else";
    case Kw_while: return "while";
    case Kw_for: return "for";
    case t_u8: return "u8";
    case t_i8: return "i8";
    case t_u16: return "u16";
    case t_i16: return "i16";
    case s_mem_lookup: return "@";
    case s_arrow: return "->";
    case s_divide: return "/";
    case s_lparen: return "(";
    case s_rparen: return ")";
    case s_lbrace: return "{";
    case s_rbrace: return "}";
    case s_semicolon: return ";";
    case s_ampersand: return "&";
    case s_comma: return ",";
    case s_plus: return "+";
    case s_minus: return "-";
    case s_equals: return "=";
    case s_star: return "*";
    case s_equalsequals: return "==";
    case s_plus_equals: return "+=";
    case s_minus_equals: return "-=";
    case s_bang: return "!";
    case s_bang_equals: return "!=";
    case s_lt: return "<";
    case s_gt: return ">";
    case s_lte: return "<=";
    case s_gte: return ">=";
    case l_num: return "NUMBER";
    case l_string: return "STRING";
    case l_identifier: return "IDENTIFIER";
    case t_eof: return "END_OF_FILE";
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

    if (token.type == t_eof || token.type == t_invalid) {
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
    case ';':
      add_token(tokens, token_count, s_semicolon, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '(':
      add_token(tokens, token_count, s_lparen, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case ')':
      add_token(tokens, token_count, s_rparen, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '{':
      add_token(tokens, token_count, s_lbrace, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '}':
      add_token(tokens, token_count, s_rbrace, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '@':
      add_token(tokens, token_count, s_mem_lookup, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '*':
      add_token(tokens, token_count, s_star, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '+':
      if (*(*ptr + 1) == '=') {
        add_token(tokens, token_count, s_plus_equals, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else {
        add_token(tokens, token_count, s_plus, line, *column, file_path, NULL);
        (*ptr)++;
      }
      return 1;
    case '-':
      if (*(*ptr + 1) == '=') {
        add_token(tokens, token_count, s_minus_equals, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else if (*(*ptr + 1) == '>') {
        add_token(tokens, token_count, s_arrow, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else {
        add_token(tokens, token_count, s_minus, line, *column, file_path, NULL);
        (*ptr)++;
      }
      return 1;
    case '/':
      add_token(tokens, token_count, s_divide, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '&':
      add_token(tokens, token_count, s_ampersand, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case ',':
      add_token(tokens, token_count, s_comma, line, *column, file_path, NULL);
      (*ptr)++;
      return 1;
    case '=':
      if (*(*ptr + 1) == '=') {
        add_token(tokens, token_count, s_equalsequals, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else {
        add_token(tokens, token_count, s_equals, line, *column, file_path, NULL);
        (*ptr)++;
      }
      return 1;
    case '!':
      if (*(*ptr + 1) == '=') {
        add_token(tokens, token_count, s_bang_equals, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else {
        add_token(tokens, token_count, s_bang, line, *column, file_path, NULL);
        (*ptr)++;
      }
      return 1;
    case '<':
      if (*(*ptr + 1) == '=') {
        add_token(tokens, token_count, s_lte, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else {
        add_token(tokens, token_count, s_lt, line, *column, file_path, NULL);
        (*ptr)++;
      }
      return 1;
    case '>':
      if (*(*ptr + 1) == '=') {
        add_token(tokens, token_count, s_gte, line, *column, file_path, NULL);
        *ptr += 2;
        column++;
      } else {
        add_token(tokens, token_count, s_gt, line, *column, file_path, NULL);
        (*ptr)++;
      }
      return 1;
    // Handle other symbols similarly...
    default:
      return 0; // Not a recognized symbol
  }
}

static int tokenize_keyword_or_identifier(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
  if (strncmp(*ptr, "fn", 2) == 0 && !isalnum((unsigned char)(*ptr)[2]) && (*ptr)[2] != '_') {
    add_token(tokens, token_count, Kw_fn, line, *column, file_path, NULL);
    *ptr += 2;
    *column += 2;
    return 1;
  } else if (strncmp(*ptr, "reg", 3) == 0 && !isalnum((unsigned char)(*ptr)[3]) && (*ptr)[3] != '_') {
    add_token(tokens, token_count, Kw_reg, line, *column, file_path, NULL);
    *ptr += 3;
    *column += 3;
    return 1;
  } else if (strncmp(*ptr, "return", 6) == 0 && !isalnum((unsigned char)(*ptr)[6]) && (*ptr)[6] != '_') {
    add_token(tokens, token_count, Kw_return, line, *column, file_path, NULL);
    *ptr += 6;
    *column += 6;
    return 1;
  } else if (strncmp(*ptr, "void", 4) == 0 && !isalnum((unsigned char)(*ptr)[4]) && (*ptr)[4] != '_') {
    add_token(tokens, token_count, Kw_void, line, *column, file_path, NULL);
    *ptr += 4;
    *column += 4;
    return 1;
  } else if (strncmp(*ptr, "if", 2) == 0 && !isalnum((unsigned char)(*ptr)[2]) && (*ptr)[2] != '_') {
    add_token(tokens, token_count, Kw_if, line, *column, file_path, NULL);
    *ptr += 2;
    *column += 2;
    return 1;
  } else if (strncmp(*ptr, "else", 4) == 0 && !isalnum((unsigned char)(*ptr)[4]) && (*ptr)[4] != '_') {
    add_token(tokens, token_count, Kw_else, line, *column, file_path, NULL);
    *ptr += 4;
    *column += 4;
    return 1;
  } else if (strncmp(*ptr, "while", 5) == 0 && !isalnum((unsigned char)(*ptr)[5]) && (*ptr)[5] != '_') {
    add_token(tokens, token_count, Kw_while, line, *column, file_path, NULL);
    *ptr += 5;
    *column += 5;
    return 1;
  } else if (strncmp(*ptr, "for", 3) == 0 && !isalnum((unsigned char)(*ptr)[3]) && (*ptr)[3] != '_') {
    add_token(tokens, token_count, Kw_for, line, *column, file_path, NULL);
    *ptr += 3;
    *column += 3;
    return 1;
  } else if (strncmp(*ptr, "u8", 2) == 0 && !isalnum((unsigned char)(*ptr)[2]) && (*ptr)[2] != '_') {
    add_token(tokens, token_count, t_u8, line, *column, file_path, NULL);
    *ptr += 2;
    *column += 2;
    return 1;
  } else if (strncmp(*ptr, "i8", 2) == 0 && !isalnum((unsigned char)(*ptr)[2]) && (*ptr)[2] != '_') {
    add_token(tokens, token_count, t_i8, line, *column, file_path, NULL);
    *ptr += 2;
    *column += 2;
    return 1;
  } else if (strncmp(*ptr, "u16", 3) == 0 && !isalnum((unsigned char)(*ptr)[3]) && (*ptr)[3] != '_') {
    add_token(tokens, token_count, t_u16, line, *column, file_path, NULL);
    *ptr += 3;
    *column += 3;
    return 1;
  } else if (strncmp(*ptr, "i16", 3) == 0 && !isalnum((unsigned char)(*ptr)[3]) && (*ptr)[3] != '_') {
    add_token(tokens, token_count, t_i16, line, *column, file_path, NULL);
    *ptr += 3;
    *column += 3;
    return 1;
  } else if (isalpha((unsigned char)**ptr) || **ptr == '_') {
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

static int tokenize_string(token_t *tokens, unsigned *token_count, char **ptr, unsigned line, unsigned *column, char *file_path) {
  if (**ptr != '"') {
    return 0; // Not a string literal
  }

  (*ptr)++; // Skip opening quote
  (*column)++;

  const char *start = *ptr;
  unsigned length = 0;
  while (**ptr != '\0' && **ptr != '"') {
    if (**ptr == '\\') {
      (*ptr)++; // Skip escape character
      (*column)++;
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
    fprintf(stderr, "Error: Unterminated string literal at %s:%u:%u\n", file_path, line, *column);
    free(string_literal);
    return -1;
  }

  (*ptr)++; // Skip closing quote
  (*column)++;

  add_token(tokens, token_count, l_string, line, *column - length - 2, file_path, string_literal);
  return 1;
}

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
    fprintf(stderr, "Error: Invalid number literal at %s:%u:%u\n", file_path, line, *column);
    return -1;
  }

  char *endptr = NULL;
  long value = strtol(literal_start, &endptr, base);
  if (endptr != (char *)end) {
    fprintf(stderr, "Error: Invalid number literal at %s:%u:%u\n", file_path, line, *column);
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
  const unsigned max_tokens = (unsigned)(file_size);
  char *ptr = (char *)source_code;
  unsigned line = 1, column = 1, token_count = 0;

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
      free_tokens(tokens, token_count);
      return NULL; // Error tokenizing string
    } else if (is_string_result > 0) {
      continue; // Successfully tokenized a string
    }

    int is_number_result = tokenize_number(tokens, &token_count, &ptr, line, &column, (char *)file_path);
    if (is_number_result < 0) {
      free_tokens(tokens, token_count);
      return NULL;
    } else if (is_number_result > 0) {
      continue;
    }

    int is_keyword_or_id_result = tokenize_keyword_or_identifier(tokens, &token_count, &ptr, line, &column, (char *)file_path);
    if (is_keyword_or_id_result < 0) {
      free_tokens(tokens, token_count);
      return NULL;
    } else if (is_keyword_or_id_result > 0) {
      continue;
    }

    ptr++;
    column++;
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