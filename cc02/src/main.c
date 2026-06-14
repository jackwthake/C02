#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "tokenizer.h"
#include "parser.h"

#define UNUSED(x) (void)(x)

static void print_help(const char *prog_name) {
  fprintf(stderr, "Usage: %s [-h] [-v] [-o output] input\n", prog_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -h            Show this help message\n");
  fprintf(stderr, "  -v            Enable verbose output (print tokens)\n");
  fprintf(stderr, "  -o output     Specify output file (not implemented yet)\n");
}

static int read_params(int argc, char * const *argv, int *verbose, char **output, char **input) {
  int opt;
  while ((opt = getopt(argc, argv, "hvo:")) != -1) {
    switch (opt) {
      case 'h':
        print_help(argv[0]);
        return 2;
      case 'v': *verbose = 1; break;
      case 'o': *output = optarg; break;
      default:
        fprintf(stderr, "Bad options: use %s -h to display help message\n", argv[0]);
        return -1;
    }
  }

  if (optind < argc) {
    *input = argv[optind];

    // ensure file ends in .c02
    const char *ext = strrchr(*input, '.');
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

static int load_file(const char *file_path, char **out_content) {
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

int main(int argc, char * const *argv) {
  int verbose = 0;
  char *output = NULL;
  char *input = NULL;
    
  UNUSED(output);
  
  /* Read command-line parameters */
  int parse_status = read_params(argc, argv, &verbose, &output, &input);
  if (parse_status != 0) {
    return parse_status == 2 ? 0 : 1;
  }

  /* Load the input file */
  char *source_code;
  long fsize;
  if ((fsize = load_file(input, &source_code)) < 0) {
    return 1;
  }
  
  /* tokenize the source code */
  unsigned num_tokens;
  token_t *tokens = tokenize(input, source_code, fsize, &num_tokens);
  if (!tokens) {
    free(source_code);
    return 1;
  }

  if (verbose) {
    print_tokens(tokens, num_tokens);
  }

  /* parse the tokens into an AST */
  parser_arena_t parser_area;
  if (!parser_init(&parser_area, PARSER_CHUNK_ALLOC_SIZE)) {
    fprintf(stderr, "Parser allocation failed.");
    free(source_code);
    free_tokens(tokens, num_tokens);
    return 1;
  }

  node_t *ast = parse(tokens, num_tokens, &parser_area);
  if (!ast) {
    fprintf(stderr, "Parsing failed\n");
    free(source_code);
    free_tokens(tokens, num_tokens);
    parser_free(&parser_area);
    return 1;
  }

  /* After parsing, tokens are no longer needed */
  free_tokens(tokens, num_tokens);

  /* Semantic analysis */

  /* After analysis, source code is no longer needed */
  free(source_code);

  /* Code generation */

  parser_free(&parser_area);

  return 0;
}