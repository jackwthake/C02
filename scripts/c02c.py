#!/usr/bin/env python3
#
# c02c - the c02 compiler driver.
#
# Orchestrates the full pipeline: c02 source -> frontend -> link/optimize
# -> codegen -> 6502 ROM.
import os
import subprocess
import sys

# `make` installs c02c into bin/ next to c02-frontend and c02-as, so they
# always sit alongside this script once installed.
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
TEMP_PATH = os.path.abspath("/tmp")

def main():
    frontend = os.path.join(SCRIPT_DIR, "c02-frontend")
    codegen = os.path.join(SCRIPT_DIR, "c02-as")

    for tool in (frontend, codegen):
        if not os.access(tool, os.X_OK):
            print(f"c02c: missing {tool} (run 'make' first)", file=sys.stderr)
            sys.exit(1)

    incremental = len([i for i in sys.argv[1:] if i == "-c"])
    output = "a.o" if incremental == 1 else "a.bin"
    if "-o" in sys.argv:
      o_index = sys.argv.index("-o")
      output = sys.argv[o_index + 1]
    
    src_files = [f for f in sys.argv[1:] if f.endswith(".c02")]
    o_files   = [f for f in sys.argv[1:] if f.endswith(".o")]
    params    = [p for p in sys.argv[1:] if "." not in p]

    if len(src_files + o_files) <= 0:
      print("Requires input files")
      sys.exit(-1)

    # --- 1. frontend: c02 source -> IR object -------------------------------
    for src in src_files:
      result = subprocess.run([frontend, src, "-o", output])
      if result.returncode != 0:
        sys.exit(result.returncode)

    if incremental:
       sys.exit(0)
    
    # --- 2. link + optimize (future OCaml pass) -----------------------------
    # TODO: merge IR objects and run optimization passes here.

    # --- 3. codegen: IR object -> 6502 ROM -----------------------------------
    # TODO: consume the frontend's emitted .o. Until that exists, run the
    for obj in o_files:
      result = subprocess.run([codegen, obj, "-o", output] + params)
      if result.returncode != 0:
        sys.exit(result.returncode)

if __name__ == "__main__":
    main()
