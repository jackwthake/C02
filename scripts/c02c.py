#!/usr/bin/env python3
#
# c02c - the c02 compiler driver.
#
# Orchestrates the full pipeline: c02 source -> frontend -> link/optimize
# -> codegen -> 6502 ROM.
import os
import subprocess
import sys
import argparse
import random
import string

# `make` installs c02c into bin/ next to c02-frontend and c02-as, so they
# always sit alongside this script once installed.
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
TEMP_PATH = os.path.abspath("/tmp")

def check_stages(stages):
  for tool in stages:
    if not os.access(tool, os.X_OK):
      print(f"c02c: missing {tool} (run 'make' first)", file=sys.stderr)
      sys.exit(1)


def parse_args():
  parser = argparse.ArgumentParser(
    prog='c02c',
    description='C02 compiler toolchain'
  )

  parser.add_argument('files', nargs='+', help='List of filenames to process')

  parser.add_argument('-c', action='store_true', help='Incremental compilation. produces object file')
  parser.add_argument('-o', '--output', help='Path for output file')

  parser.add_argument('--parse-only', action='store_true', help='Check syntax only, produce no output')
  parser.add_argument('--dump-ast', action='store_true', help='Print AST output after parsing, produce no output file')

  parser.add_argument('--dump-ir', action='store_true', help='Print IR output, produce no output file')
  parser.add_argument('--strip-debug', action='store_true', help='Strip debug symbols from final binary')

  args = parser.parse_args()
  src_files = [f for f in args.files if f.endswith(".c02")]
  o_files   = [f for f in args.files if f.endswith(".o")]
  return (args, src_files, o_files)


def make_temp_object():
  random_string = ''.join(random.choices(string.ascii_letters + string.digits, k=10)) + '.o'
  return os.path.join(TEMP_PATH, random_string)


def clean_o_file_temps(o_files):
  for f in o_files:
    os.remove(f)


def main():
  (args, src_files, o_files) = parse_args()

  frontend = os.path.join(SCRIPT_DIR, "c02-frontend")
  linker = os.path.join(SCRIPT_DIR, "c02-ld")
  codegen = os.path.join(SCRIPT_DIR, "c02-as")

  check_stages([frontend, linker, codegen])

  frontend_args = []
  if args.parse_only: frontend_args += ['--parse-only']
  if args.dump_ast: frontend_args += ['--dump-ast']

  output = "a.out" if args.output == None else args.output
  temps = []

  # --- 1. frontend: c02 source -> IR object -------------------------------
  for src in src_files:
    if args.c:
      src_out = output
    else:
      src_out = make_temp_object()
      temps += [src_out] # collect temps for deletion after pipeline
    
    o_files += [src_out]
    result = subprocess.run([frontend] + frontend_args + ['-o', src_out] + [src])
    if result.returncode != 0:
      sys.exit(result.returncode)

  if args.c or args.parse_only or args.dump_ast:
    clean_o_file_temps(temps)
    sys.exit(0)
  
  # --- 2. link + optimize (future OCaml pass) -----------------------------
  # TODO: merge IR objects and run optimization passes here.
  result = subprocess.run([linker] + o_files)
  if result.returncode != 0:
    sys.exit(result.returncode)

  linked = o_files[0] # TODO: this will be replaced with the result of the linking stage

  generater_args = []
  if args.dump_ir: generater_args += ['--dump-ir']
  if args.strip_debug: frontend_args += ['--strip-debug']

  # --- 3. codegen: IR object -> 6502 ROM -----------------------------------
  # TODO: consume the frontend's emitted .o. Until that exists, run the
  result = subprocess.run([codegen, linked, "-o", output] + generater_args)

  clean_o_file_temps(temps)
  if result.returncode != 0:
    sys.exit(result.returncode)

if __name__ == "__main__":
  main()
