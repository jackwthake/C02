#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "tokenizer.h"

#define UNUSED(x) (void)(x)

static int read_params(int argc, char * const *argv, int *verbose, char **output, char **input) {
  int opt;
  while ((opt = getopt(argc, argv, "vo:")) != -1) {
    switch (opt) {
      case 'v': *verbose = 1; break;
      case 'o': *output = optarg; break;
      default: 
        fprintf(stderr, "Usage: %s [-v] [-o output] input\n", argv[0]);
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
    fprintf(stderr, "Usage: %s [-v] [-o output] input\n", argv[0]);
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

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *content = malloc(fsize + 1);
  if (!content) {
    perror("Failed to allocate memory for file content");
    fclose(f);
    return -1;
  }

  fread(content, 1, fsize, f);
  content[fsize] = 0;
  fclose(f);

  *out_content = content;
  return 0;
}

int main(int argc, char * const *argv) {
  int verbose = 0;
  char *output = NULL;
  char *input = NULL;
    
  UNUSED(verbose); // UNIMPLEMENTED
  UNUSED(output);
  
  /* Read command-line parameters */
  if (read_params(argc, argv, &verbose, &output, &input) != 0) {
    return 1;
  }

  /* Load the input file */
  char *source_code;
  if (load_file(input, &source_code) != 0) {
    return 1;
  }
  
  /* tokenize the source code */
  token_t *tokens = tokenize(input, source_code);

  /* free the allocated memory */
  free(source_code);
  free(tokens);

  return 0;
}