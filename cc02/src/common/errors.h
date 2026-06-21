#ifndef __ERRORS_H__
#define __ERRORS_H__

/*
 * errors.h - shared error representation and printing
 * -------------------------------------------------------
 * error_t is used by both the parser and the analyzer. The two stages have
 * different error shapes (a parser error is "found this token, expected
 * that" - a semantic error is "this type didn't match that type", "this
 * name was already declared", etc) so error_t is a tagged union: one
 * `loc` shared by every variant (every error happened *somewhere*), plus
 * a per-kind payload in the union.
 *
 * PARSER VS ANALYZER ERROR HANDLING
 * ----------------------------------
 * The parser bails at its first error - a syntax error leaves the token
 * stream position ambiguous, so there's no reliable way to keep parsing.
 * The analyzer does NOT bail at its first error - a semantic error (an
 * undeclared identifier, a type mismatch) doesn't corrupt the AST, so the
 * analyzer keeps walking and accumulates every error it finds, the same
 * way tokenize() keeps scanning after a bad token and reports a count at
 * the end. error_t itself doesn't know or care which stage produced it -
 * that "bail vs accumulate" behaviour lives in parser.c/analyzer.c, not
 * here.
 *
 * print_error() is the single shared printer for every error_t, regardless
 * of which stage produced it - this replaces the old print_parse_error()
 * (formerly in parser_print.c, parser-only).
 */

#include "tokenizer.h"   // token_t, token_location_t
#include "types.h"       // type_t


typedef enum {
  // parser errors
  ERR_UNEXPECTED_EOF,
  ERR_UNEXPECTED_TOKEN,
  ERR_ALLOCATION_FAILED,

  // semantic errors
  ERR_UNDECLARED_IDENTIFIER,
  ERR_NOT_A_FUNCTION,
  ERR_UNKNOWN_STRUCT,
  ERR_REDECLARATION,
  ERR_TYPE_MISMATCH,
  ERR_WRONG_ARG_TYPE,
  ERR_WRONG_ARG_COUNT,
  ERR_UNKNOWN_FIELD,
  ERR_NOT_ASSIGNABLE,
  ERR_MISSING_MAIN,         // no main, or a `main` that isn't a function
} error_type_t;

typedef struct {
  error_type_t type;
  token_location_t loc;     // every error has one, regardless of kind

  union {
    struct {
      token_t found;
      char *expected;
      char *context;
    } parse;                          // ERR_UNEXPECTED_EOF, ERR_UNEXPECTED_TOKEN, ERR_ALLOCATION_FAILED

    struct {
      char *name;
    } name_error;                     // ERR_UNDECLARED_IDENTIFIER, ERR_NOT_A_FUNCTION, ERR_UNKNOWN_STRUCT, ERR_REDECLARATION, ERR_NOT_ASSIGNABLE

    struct {
      type_t expected;
      type_t actual;
      char *context;                  // e.g. "variable 'x'", "return statement", "argument 2 of 'foo'"
    } type_mismatch;                  // ERR_TYPE_MISMATCH, ERR_WRONG_ARG_TYPE

    struct {
      char *fn_name;
      unsigned expected_count;
      unsigned actual_count;
    } arg_count;                      // ERR_WRONG_ARG_COUNT

    struct {
      char *struct_name;
      char *field_name;
    } unknown_field;                  // ERR_UNKNOWN_FIELD
  };
} error_t;


// Prints `e` to stderr in the project's standard format (file:line:col,
// message, source line + caret via print_error_line(), and for parser
// errors the expected/context lines). Handles every error_type_t variant -
// this is the only place that needs to know how to render an error_t.
void print_error(error_t *e);

#endif // __ERRORS_H__