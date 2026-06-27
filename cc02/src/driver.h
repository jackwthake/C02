#ifndef __DRIVER_H__
#define __DRIVER_H__

#define PARAM_ERROR_RET_CODE 1
#define FILE_LOAD_ERROR_RET_CODE 2
#define TOKEN_ERROR_RET_CODE 3
#define PARSER_ERROR_RET_CODE 4
#define ANALYZER_ERROR_RET_CODE 5
#define IR_ERROR_RET_CODE 6
#define CODE_GEN_ERROR_RET_CODE 7

typedef struct {
  int dump_tokens;
  int dump_ast;
  int dump_symbols;
  int dump_ir;
  int syntax_only;
  int time_report;
  int incremental_build;
  int strip_debug;

  int is_input_bin;
  char *output;
  char *input;
} params_t;

typedef struct {
  double load;
  double lex;
  double parse;
  double sema;
  double ir;
  double codegen;
  double cleanup;
} timing_t;

double get_time_ms(void);
int run_compiler(params_t *params, timing_t *timing);

#endif
