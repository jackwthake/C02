#!/usr/bin/env python3
#
# c02c - the c02 compiler driver.
#
# Orchestrates the full pipeline: c02 source -> frontend -> link/optimize
# -> codegen -> 6502 ROM.
#
# This is a stub. The Haskell frontend does not yet accept arguments or emit
# an IR object, so the frontend -> codegen handoff is still a TODO. The
# backend (codegen) already works, so any arguments passed to c02c are handed
# straight to it for now - e.g. `c02c prog.o -o prog.bin` runs codegen for real.
import os
import subprocess
import sys

# `make` installs c02c into bin/ next to c02-frontend and c02-as, so they
# always sit alongside this script once installed.
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))


def main():
    frontend = os.path.join(SCRIPT_DIR, "c02-frontend")
    codegen = os.path.join(SCRIPT_DIR, "c02-as")

    for tool in (frontend, codegen):
        if not os.access(tool, os.X_OK):
            print(f"c02c: missing {tool} (run 'make' first)", file=sys.stderr)
            sys.exit(1)

    # --- 1. frontend: c02 source -> IR object -------------------------------
    # TODO: forward sys.argv[1:] (the .c02 input and -o output) once the
    # frontend parses arguments and writes a .o. For now it ignores them and
    # just runs.
    result = subprocess.run([frontend])
    if result.returncode != 0:
        sys.exit(result.returncode)

    # --- 2. link + optimize (future OCaml pass) -----------------------------
    # TODO: merge IR objects and run optimization passes here.

    # --- 3. codegen: IR object -> 6502 ROM -----------------------------------
    # TODO: consume the frontend's emitted .o. Until that exists, run the
    # backend directly on whatever was passed to c02c (e.g. an existing IR
    # object).
    if len(sys.argv) > 1:
        result = subprocess.run([codegen] + sys.argv[1:])
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
