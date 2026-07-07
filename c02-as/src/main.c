/*
 * c02-as - the c02 backend: IR object file -> 6502 ROM.
 *
 * This is the entire tool. It reads a serialized IR object (.o) produced
 * by the frontend, hands the loaded module to the code generator, and
 * writes the resulting ROM image. No source, lexer, parser, or analysis
 * lives here - that is the frontend's job.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>

#include "ir.h"
#include "generator.h"
#include "colors.h"

#define PARAM_ERROR_RET_CODE    1
#define IR_ERROR_RET_CODE       2
#define CODE_GEN_ERROR_RET_CODE 3

typedef struct {
  int   dump_ir;
  int   strip_debug;
  char *output;
  char *input;
} params_t;


static void print_help(const char *prog_name) {
  fprintf(stderr, "Usage: %s [-h] [--dump-ir] [--strip-debug] [-o output] input.o\n", prog_name);
  fprintf(stderr, "\n");
  fprintf(stderr, "  input                Serialized IR object file (.o)\n");
  fprintf(stderr, "  -o, --output         Output ROM file (default: a.bin)\n");
  fprintf(stderr, "  --dump-ir            Print the loaded IR before code generation\n");
  fprintf(stderr, "  --strip-debug        Omit the symbol table from the output ROM\n");
  fprintf(stderr, "  -h, --help           Show this message\n");
}


// Returns 0 to proceed, 2 for --help, -1 on bad arguments.
static int read_params(int argc, char *const *argv, params_t *params) {
  static const struct option long_options[] = {
    {"help",        no_argument,       0, 'h'},
    {"dump-ir",     no_argument,       0,  1 },
    {"strip-debug", no_argument,       0,  2 },
    {"output",      required_argument, 0, 'o'},
    {0, 0, 0, 0},
  };

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "ho:", long_options, &option_index)) != -1) {
    switch (opt) {
      case 'h': print_help(argv[0]);      return 2;
      case 'o': params->output = optarg;  break;
      case 1:   params->dump_ir = 1;      break;
      case 2:   params->strip_debug = 1;  break;
      default:
        fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
        return -1;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
    fprintf(stderr, "Input IR object file is required\n");
    return -1;
  }

  params->input = argv[optind];
  return 0;
}


static int run(const params_t *params) {
  ir_gen_t gen;

  if (!ir_read(&gen, params->input)) {
    fprintf(stderr, RED "Failed to read IR from %s\n" RESET, params->input);
    return IR_ERROR_RET_CODE;
  }

  if (params->dump_ir)
    ir_gen_print(&gen);

  size_t rom_size;
  uint8_t *rom = generate_rom(&gen, &rom_size, !params->strip_debug);
  if (!rom) {
    fprintf(stderr, RED "Code generation failed.\n" RESET);
    ir_gen_free(&gen);
    return CODE_GEN_ERROR_RET_CODE;
  }

  int status = 0;
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
  // Codegen reads strings/arrays that live in gen's arena, so it must
  // outlive generate_rom - only now is it safe to tear the arena down.
  ir_gen_free(&gen);
  return status;
}


int main(int argc, char *const *argv) {
  ENABLE_COLORS();

  params_t params = {0};
  int parse_status = read_params(argc, argv, &params);
  if (parse_status != 0)
    return parse_status == 2 ? 0 : PARAM_ERROR_RET_CODE;

  return run(&params);
}
