#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver.h"
#include "colors.h"
#include "tokenizer.h"
#include "parser.h"
#include "analyzer.h"
#include "ir.h"
#include "generator.h"


typedef struct {
  char *source_code;
  long source_size;
  token_t *tokens;
  unsigned num_tokens;
  parser_t parser;
  analyzer_t analyzer;
  ir_gen_t ir_gen;
  ast_t ast;
} compiler_t;


double get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}


static void compiler_cleanup(compiler_t *c) {
  free(c->source_code);
  if (c->tokens) free_tokens(c->tokens, c->num_tokens);
  parser_free(&c->parser);
  analyzer_free(&c->analyzer);
  ir_gen_free(&c->ir_gen);
}


static long load_file(const char *file_path, char **out_content, int is_bin) {
  FILE *f = fopen(file_path, is_bin ? "rb" : "r");
  if (!f) {
    perror("Failed to open input file");
    return -1;
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    perror("Failed to seek input file");
    fclose(f);
    return -1;
  }

  long fsize = ftell(f);
  if (fsize < 0) {
    perror("Failed to determine input file size");
    fclose(f);
    return -1;
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    perror("Failed to rewind input file");
    fclose(f);
    return -1;
  }

  char *content = malloc((size_t)fsize + 1);
  if (!content) {
    perror("Failed to allocate memory for file content");
    fclose(f);
    return -1;
  }

  size_t bytes_read = fread(content, 1, (size_t)fsize, f);
  if (bytes_read != (size_t)fsize) {
    perror("Failed to read input file");
    free(content);
    fclose(f);
    return -1;
  }

  content[fsize] = '\0';
  fclose(f);

  *out_content = content;
  return fsize + 1;
}


static int load_source(params_t *params, compiler_t *c, timing_t *t) {
  double start = get_time_ms();
  c->source_size = load_file(params->input, &c->source_code, params->is_input_bin);
  t->load = get_time_ms() - start;

  if (c->source_size < 0) return FILE_LOAD_ERROR_RET_CODE;
  return 0;
}


static int run_frontend(params_t *params, compiler_t *c, timing_t *t) {
  double start = get_time_ms();
  c->tokens = tokenize(params->input, c->source_code, c->source_size, &c->num_tokens);
  t->lex = get_time_ms() - start;

  if (!c->tokens) return TOKEN_ERROR_RET_CODE;
  if (params->dump_tokens) print_tokens(c->tokens, c->num_tokens);

  start = get_time_ms();
  if (!parser_init(&c->parser)) {
    fprintf(stderr, "Parser allocation failed.\n");
    return PARSER_ERROR_RET_CODE;
  }
  c->ast = parse(&c->parser, c->tokens, c->num_tokens);
  t->parse = get_time_ms() - start;

  if (!c->ast) return PARSER_ERROR_RET_CODE;
  if (params->dump_ast) print_ast(c->ast);

  start = get_time_ms();
  if (!analyzer_init(&c->analyzer)) return ANALYZER_ERROR_RET_CODE;
  symtab_t *symbol_table = analyze(&c->analyzer, c->ast);
  t->sema = get_time_ms() - start;

  if (!symbol_table || c->analyzer.errors || is_symtab_empty(symbol_table)) {
    if (c->analyzer.errors > 0) {
      fprintf(stderr, RED "\nSemantic analysis failed with " BOLD_RED "%u" RESET RED " errors.\n" RESET, c->analyzer.errors);
    }
    return ANALYZER_ERROR_RET_CODE;
  }

  if (params->dump_symbols) print_symtab(symbol_table);
  return 0;
}


static int run_ir(params_t *params, compiler_t *c, timing_t *t) {
  double start = get_time_ms();
  int status = 0;

  if (params->is_input_bin) {
    if (!ir_read(&c->ir_gen, params->input)) {
      fprintf(stderr, "Failed to read IR from %s\n", params->input);
      t->ir = get_time_ms() - start;
      return IR_ERROR_RET_CODE;
    }
  } else {
    if (!ir_gen_init(&c->ir_gen)) {
      fprintf(stderr, "IR generator allocation failed.\n");
      t->ir = get_time_ms() - start;
      return IR_ERROR_RET_CODE;
    }
    if (!ir_gen_run(&c->ir_gen, c->ast)) {
      fprintf(stderr, RED "IR generation failed.\n" RESET);
      t->ir = get_time_ms() - start;
      return IR_ERROR_RET_CODE;
    }
  }

  if (params->incremental_build) {
    const char *out = params->output ? params->output : "a.o";
    if (!ir_write(&c->ir_gen, out)) {
      fprintf(stderr, "Failed to write IR to %s\n", out);
      status = IR_ERROR_RET_CODE;
    }
  }

  t->ir = get_time_ms() - start;
  if (params->dump_ir) ir_gen_print(&c->ir_gen);

  return status;
}


static int run_codegen(params_t *params, compiler_t *c, timing_t *t) {
  double start = get_time_ms();
  int status = 0;

  size_t rom_size;
  uint8_t *rom = generate_rom(&c->ir_gen, &rom_size, !params->strip_debug);
  if (!rom) {
    fprintf(stderr, RED "Code generation failed.\n" RESET);
    t->codegen = get_time_ms() - start;
    return CODE_GEN_ERROR_RET_CODE;
  }

  const char *out = params->output ? params->output : "a.bin";
  FILE *file = fopen(out, "wb");
  if (!file) {
    fprintf(stderr, "Failed to open output file %s\n", out);
    status = CODE_GEN_ERROR_RET_CODE;
  } else {
    size_t written = fwrite(rom, 1, rom_size, file);
    fclose(file);
    if (written != rom_size) {
      fprintf(stderr, "Failed to write output file %s\n", out);
      status = CODE_GEN_ERROR_RET_CODE;
    }
  }

  free(rom);
  t->codegen = get_time_ms() - start;
  return status;
}


int run_compiler(params_t *params, timing_t *timing) {
  compiler_t c = {0};

  int status = load_source(params, &c, timing);

  if (status == 0 && !params->is_input_bin) {
    // inject compiler globals;
    const char *externs = "\ndecl u16 __heap_start;\ndecl u16 __memory_top;\n";
    size_t extern_length = strlen(externs);

    c.source_size += (long)extern_length;
    char *new_source = realloc(c.source_code, (size_t)c.source_size);
    if (!new_source) {
      compiler_cleanup(&c);
      return TOKEN_ERROR_RET_CODE;
    }
    
    c.source_code = new_source;
    strcat(c.source_code, externs);

    status = run_frontend(params, &c, timing);
  }

  if (status == 0 && !params->syntax_only)
    status = run_ir(params, &c, timing);

  int any_dump = params->dump_tokens || params->dump_ast
               || params->dump_symbols || params->dump_ir;
  if (status == 0 && !params->syntax_only && !params->incremental_build && !any_dump)
    status = run_codegen(params, &c, timing);

  double cleanup_start = get_time_ms();
  compiler_cleanup(&c);
  timing->cleanup = get_time_ms() - cleanup_start;

  return status;
}
