#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h> 

#include "colors.h"

#include "tokenizer.h"
#include "parser.h"

#define UNUSED(x) (void)(x)

typedef struct params_t {
  int dump_tokens;
  int dump_ast;
  int syntax_only;
  int time_report;
  char *output;
  char *input;
} params_t;


static void print_help(const char *prog_name) {
  fprintf(stderr, "Usage: %s [-h] [--token-dump] [--ast-dump] [-o output] input\n", prog_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -h, --help           Show this help message\n");
  fprintf(stderr, "  --token-dump         Dump the token list after tokenization\n");
  fprintf(stderr, "  --ast-dump           Dump the AST using print_ast after parsing\n");
  fprintf(stderr, "  --syntax-check-only  Stop after syntax and semantic checks\n");
  fprintf(stderr, "  --time-report        Prints a report showing how long each stage of compilation took\n");
  fprintf(stderr, "  -o, --output         Specify output file (not implemented yet)\n");
}


static int read_params(int argc, char * const *argv, params_t *params) {
  static const struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"token-dump", no_argument, 0, 1},
    {"ast-dump", no_argument, 0, 2},
    {"syntax-check-only", no_argument, 0, 3},
    {"time-report", no_argument, 0, 4},
    {"output", required_argument, 0, 'o'},
    {0, 0, 0, 0},
  };

  int opt;
  int option_index = 0;

  while ((opt = getopt_long(argc, argv, "ho:", long_options, &option_index)) != -1) {
    switch (opt) {
      case 'h':
        print_help(argv[0]);
        return 2;
      case 'o':
        params->output = optarg;
        break;
      case 1:
        params->dump_tokens = 1;
        break;
      case 2:
        params->dump_ast = 1;
        break;
      case 3:
        params->syntax_only = 1;
        break;
      case 4:
        params->time_report = 1;
        break;
      default:
        fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
        return -1;
    }
  }

  if (optind < argc) {
    params->input = argv[optind];

    // ensure file ends in .c02
    const char *ext = strrchr(params->input, '.');
    if (!ext || strcmp(ext, ".c02") != 0) {
      fprintf(stderr, "Input file must have .c02 extension\n");
      return -1;
    }
  } else {
    fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
    fprintf(stderr, "Input file is required\n");
    return -1;
  }

  return 0;
}


static long load_file(const char *file_path, char **out_content) {
  FILE *f = fopen(file_path, "r");
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

  const size_t bytes_read = fread(content, 1, (size_t)fsize, f);
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

/* * Helper function to get the current time in milliseconds.
 * Uses CLOCK_MONOTONIC to protect against system clock changes.
 */
static double get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}


int main(int argc, char * const *argv) {
  ENABLE_COLORS();
  
  params_t params = {0};

  /* Read command-line parameters */
  int parse_status = read_params(argc, argv, &params);
  if (parse_status != 0) {
    return parse_status == 2 ? 0 : 1;
  }

  /* Timing variables */
  double t_total_start = get_time_ms();
  double t_step_start;
  double t_load = 0.0, t_lex = 0.0, t_parse = 0.0, t_sema = 0.0, t_codegen = 0.0, t_cleanup = 0.0;

  /* Load the input file */
  t_step_start = get_time_ms();
  char *source_code;
  long fsize;
  if ((fsize = load_file(params.input, &source_code)) < 0) {
    return 1;
  }
  t_load = get_time_ms() - t_step_start;
  
  /* tokenize the source code */
  t_step_start = get_time_ms();
  unsigned num_tokens;
  token_t *tokens = tokenize(params.input, source_code, fsize, &num_tokens);
  t_lex = get_time_ms() - t_step_start; /* Exclude I/O dump time */
  
  if (!tokens) {
    free(source_code);
    return 1;
  }

  if (params.dump_tokens) {
    print_tokens(tokens, num_tokens);
  }

  /* parse the tokens into an AST */
  t_step_start = get_time_ms();
  parser_arena_t parser_area;
  if (!parser_init(&parser_area, PARSER_CHUNK_ALLOC_SIZE)) {
    fprintf(stderr, "Parser allocation failed.");
    free(source_code);
    free_tokens(tokens, num_tokens);
    return 1;
  }

  node_t *ast = parse(tokens, num_tokens, &parser_area);
  t_parse = get_time_ms() - t_step_start; /* Exclude I/O dump time */

  if (!ast) {
    free(source_code);
    free_tokens(tokens, num_tokens);
    parser_free(&parser_area);
    return 1;
  }

  if (params.dump_ast) {
    print_ast(ast);
  }
  
  /* Semantic analysis */
  t_step_start = get_time_ms();
  /* After analysis, source code is no longer needed */
  free(source_code);
  t_sema = get_time_ms() - t_step_start;
  
  if (params.syntax_only) {
    goto finish;
  }

  /* Code generation */
  t_step_start = get_time_ms();

  t_codegen = get_time_ms() - t_step_start;

finish:

  t_step_start = get_time_ms();
  free_tokens(tokens, num_tokens);
  parser_free(&parser_area);
  t_cleanup = get_time_ms() - t_step_start;

  /* Print Timing Report if flag was passed */
  if (params.time_report) {
    double t_total = get_time_ms() - t_total_start;
    printf("\n=== Compilation Time Report ===\n");
    printf("File Load:      %8.3f ms\n", t_load);
    printf("Tokenization:   %8.3f ms\n", t_lex);
    printf("Parsing:        %8.3f ms\n", t_parse);
    printf("Sem. Analysis:  %8.3f ms\n", t_sema);
    
    if (params.syntax_only) {
      printf("Code Gen:       %8s\n", "Skipped");
    } else {
      printf("Code Gen:       %8.3f ms\n", t_codegen);
    }
    printf("Cleanup:        %8.3f ms\n", t_cleanup);
    printf("-------------------------------\n");
    printf("Total Time:     %8.3f ms\n", t_total);
    printf("===============================\n\n");
  }

  return 0;
}