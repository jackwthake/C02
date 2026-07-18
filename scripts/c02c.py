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
STANDARD_LIBRARY = os.path.join(SCRIPT_DIR, "lib/libc02.o")
INCLUDE_DIR = os.path.join(SCRIPT_DIR, "include/")

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
  parser.add_argument('-I', dest='include_dirs', action='append', default=[], metavar='dir',
                      help='Add a directory to the header include search path (repeatable)')

  parser.add_argument('--parse-only', action='store_true', help='Check syntax only, produce no output')
  parser.add_argument('--dump-ast', action='store_true', help='Print AST output after parsing, produce no output file')

  parser.add_argument('--no-stdlib', action='store_true', help="Don't link with the standard library")
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
    # A temp path is registered before its file is written, so a stage that
    # fails early can leave a path here that never became a file — tolerate it.
    try:
      os.remove(f)
    except FileNotFoundError:
      pass


def main():
  (args, src_files, o_files) = parse_args()

  frontend = os.path.join(SCRIPT_DIR, "c02-frontend")
  linker = os.path.join(SCRIPT_DIR, "c02-ld")
  codegen = os.path.join(SCRIPT_DIR, "c02-as")

  check_stages([frontend, linker, codegen])

  args.include_dirs += [INCLUDE_DIR]

  if not args.no_stdlib:
    o_files += [STANDARD_LIBRARY]

  frontend_args = []
  if args.parse_only: frontend_args += ['--parse-only']
  if args.dump_ast: frontend_args += ['--dump-ast']
  for d in args.include_dirs: frontend_args += ['-I', d]  # header search dirs, in order

  output = "a.out" if args.output == None else args.output
  temps = []

  # The pipeline registers temp objects in `temps` as it goes; the finally
  # clause deletes them on every exit path — success, a failing stage's
  # sys.exit (which raises SystemExit and passes through finally), or a crash.
  try:
    # --- 1. frontend: c02 source -> IR object -----------------------------
    for src in src_files:
      src_out = make_temp_object()
      temps += [src_out] # collect temps for deletion after pipeline
      o_files += [src_out]
      result = subprocess.run([frontend] + frontend_args + ['-o', src_out] + [src])
      if result.returncode != 0:
        sys.exit(result.returncode)

    if args.parse_only or args.dump_ast:
      sys.exit(0)

    # --- 2. link: merge every object into one -----------------------------
    # With -c we stop here and write the merged object to the requested -o. It
    # may be a library with no main, which is fine: the generator, not the
    # linker, requires an entry point. Without -c the merged object feeds codegen.
    if args.c:
      result = subprocess.run([linker] + o_files + ["-o", output])
      sys.exit(result.returncode)

    if len(o_files) > 1: # more than one object: merge them
      linked = make_temp_object()
      temps += [linked]
      result = subprocess.run([linker] + o_files + [ "-o", linked])
      if result.returncode != 0:
        sys.exit(result.returncode)
    else:
      linked = o_files[0] # single object nothing to merge

    generater_args = []
    if args.dump_ir: generater_args += ['--dump-ir']
    if args.strip_debug: generater_args += ['--strip-debug']

    # --- 3. codegen: IR object -> 6502 ROM --------------------------------
    result = subprocess.run([codegen, linked, "-o", output] + generater_args)
    if result.returncode != 0:
      sys.exit(result.returncode)
  finally:
    clean_o_file_temps(temps)

if __name__ == "__main__":
  main()
