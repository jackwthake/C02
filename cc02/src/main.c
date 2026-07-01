#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include "driver.h"
#include "colors.h"


static void print_help(const char *prog_name) {
  fprintf(stderr, "Usage: %s [-h] [--token-dump] [--ast-dump] [-o output] input\n", prog_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -h, --help           Show this help message\n");
  fprintf(stderr, "  --token-dump         Dump the token list after tokenization\n");
  fprintf(stderr, "  --ast-dump           Dump the AST using print_ast after parsing\n");
  fprintf(stderr, "  --symbol-dump        Dump the Symbol Table after analysis\n");
  fprintf(stderr, "  --ir-dump            Dump the IR after lowering\n");
  fprintf(stderr, "  --syntax-check-only  Stop after syntax and semantic checks\n");
  fprintf(stderr, "  --time-report        Prints a report showing how long each stage of compilation took\n");
  fprintf(stderr, "  --strip-debug        Omit symbol table from output binary\n");
  fprintf(stderr, "  -c,                  Incremental compile, generate object file\n");
  fprintf(stderr, "  -o, --output         Specify output file\n");
}


static int read_params(int argc, char * const *argv, params_t *params) {
  static const struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"token-dump", no_argument, 0, 1},
    {"ast-dump", no_argument, 0, 2},
    {"symbol-dump", no_argument, 0, 3},
    {"ir-dump", no_argument, 0, 6},
    {"syntax-check-only", no_argument, 0, 4},
    {"time-report", no_argument, 0, 5},
    {"strip-debug", no_argument, 0, 7},
    {"output", required_argument, 0, 'o'},
    {0, 0, 0, 0},
  };

  int opt;
  int option_index = 0;

  while ((opt = getopt_long(argc, argv, "ho:c", long_options, &option_index)) != -1) {
    switch (opt) {
      case 'h':
        print_help(argv[0]);
        return 2;
      case 'o':
        params->output = optarg;
        break;
      case 'c':
        params->incremental_build = 1;
        break;
      case 1:
        params->dump_tokens = 1;
        break;
      case 2:
        params->dump_ast = 1;
        break;
      case 3:
        params->dump_symbols = 1;
        break;
      case 4:
        params->syntax_only = 1;
        break;
      case 6:
        params->dump_ir = 1;
        break;
      case 5:
        params->time_report = 1;
        break;
      case 7:
        params->strip_debug = 1;
        break;
      default:
        fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
        return -1;
    }
  }

  if (optind < argc) {
    params->input = argv[optind];

    const char *ext = strrchr(params->input, '.');
    if (strcmp(ext, ".o") == 0 || strcmp(ext, ".out") == 0) {
      params->is_input_bin = 1;
    } else if (!ext || strcmp(ext, ".c02") != 0) {
      fprintf(stderr, "Input file must have .c02 extension\n");
      return -1;
    } else {
      params->is_input_bin = 0;
    }
  } else {
    fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
    fprintf(stderr, "Input file is required\n");
    return -1;
  }

  if (params->incremental_build && params->syntax_only) {
    params->incremental_build = 0;
    fprintf(stderr, "Warning: argument '-c' ignored because '--syntax-check-only' was specified\n");
  }

  if (params->output && params->syntax_only) {
    fprintf(stderr, "Warning: argument '-o', '--output' ignored because '--syntax-check-only' was specified\n");
  }

  return 0;
}


static void print_timing_report(const params_t *params, const timing_t *timing, double total) {
  printf("\n=== Compilation Time Report ===\n");
  printf("File Load:      %8.3f ms\n", timing->load);

  if (!params->is_input_bin) {
    printf("Tokenization:   %8.3f ms\n", timing->lex);
    printf("Parsing:        %8.3f ms\n", timing->parse);
    printf("Sem. Analysis:  %8.3f ms\n", timing->sema);
  }

  if (params->syntax_only) {
    printf("IR Generation:  %8s\n", "Skipped");
    printf("Code Gen:       %8s\n", "Skipped");
  } else {
    printf("IR Generation:  %8.3f ms\n", timing->ir);
    printf("Code Gen:       %8.3f ms\n", timing->codegen);
  }

  printf("Cleanup:        %8.3f ms\n", timing->cleanup);
  printf("-------------------------------\n");
  printf("Total Time:     %8.3f ms\n", total);
  printf("===============================\n\n");
}


int main(int argc, char * const *argv) {
  ENABLE_COLORS();

  params_t params = {0};
  int parse_status = read_params(argc, argv, &params);
  if (parse_status != 0) {
    return parse_status == 2 ? 0 : PARAM_ERROR_RET_CODE;
  }

  timing_t timing = {0};
  double t_total_start = get_time_ms();

  int status = run_compiler(&params, &timing);

  if (params.time_report) {
    double t_total = get_time_ms() - t_total_start;
    print_timing_report(&params, &timing, t_total);
  }

  return status;
}
