# Contributing to C02

Thanks for taking a look at C02! The project is in early, active development -
the workflow below is intentionally lightweight, but consistent.

## Project Status

C02 compiles through a split toolchain: a Haskell frontend (`c02-frontend`:
tokenizer, parser, analyzer, IR generation) and a C code generator (`c02-as`)
that emits working 65C02 ROMs, driven by `c02c` — see the [README's Current
Status & Limitations](./README.md#current-status--limitations) for what works
today and what's still unimplemented. The toolchain is under active
development, so expect internals (especially the IR and codegen) to keep
shifting.

## License

C02 is licensed under GPLv3 (see [LICENSE](./LICENSE)), with an explicit
exception so that programs compiled *with* C02 aren't subject to GPL
obligations themselves — only the compiler's own source is covered. By
contributing, you agree your contribution is licensed under the same
terms.

## Workflow

1. **Open an issue first for anything non-trivial.** A quick description of
   the bug or feature before writing code avoids duplicated effort and gives
   a place to discuss design before it's locked into a PR.

2. **Branch off `main`.** Use a short, descriptive name with a prefix
   indicating the kind of change:

   | Prefix      | Use for                                  |
   |-------------|-------------------------------------------|
   | `feat/`     | New language features, codegen additions  |
   | `fix/`      | Bug fixes                                  |
   | `refactor/` | Internal restructuring, no behavior change |
   | `docs/`     | README, comments, this file, etc.          |
   | `test/`     | Test additions or test harness changes     |

   ```bash
   git checkout -b feat/array-types
   ```

3. **Commit as you go.** Work-in-progress commits on your branch don't need
   to be clean - history gets squashed on merge (see below).

4. **Add tests.** There are two kinds, and a change should come with whichever
   fits — frontend changes get golden tests, codegen changes get emulator tests.

   **Golden-file tests** (frontend), run by `scripts/test.py`, grouped by stage:

   ```
   test/<stage>/*.c02             one program per file
   test/<stage>/golden/*.golden   the expected output, one per input
   ```

   `test/parser/` and `test/analyzer/` are both wired up (invoked through
   `c02-frontend`); a new stage slots in via the `STAGES` map in
   `scripts/test.py`.

   - A **positive** case (any name) must succeed: the frontend exits 0 and prints
     the AST to stdout, which the `.golden` captures.
   - A **negative** case must be named `bad_*.c02`: it must *fail*, so the
     frontend exits nonzero and writes a diagnostic to stderr, which the
     `.golden` captures. Keep each `bad_` case down to a single error.

   ```bash
   scripts/test.py                 # build, then run every stage
   scripts/test.py parser          # run one stage
   scripts/test.py --bless         # (re)generate goldens after an intended change
   scripts/test.py -v              # show a diff for every mismatch
   ```

   Always eyeball a `--bless` diff before committing - blessing enshrines
   whatever the compiler currently prints. A nonzero exit on a positive case is
   reported as a `CRASH` (never blessed); a `bad_` case that succeeds is an
   `XPASS`. Bug fixes should add a regression case where practical.

   **Emulator tests** (code generator), under `test/emu/`, run by `make emu-test`
   (or `test/emu/run.py`). Each `.c02` program is compiled to a 32 KB ROM,
   executed in a py65 65C02 emulator, and checked against `// EXPECT:` directives
   on its final memory/CPU state. Add one for any codegen change; `run.py
   --ir-on-fail` shows whether a failure is in frontend lowering or codegen. See
   [`test/emu/README.md`](./test/emu/README.md) for the directive format.

5. **Update the changelog.** Add a line under `[Unreleased]` in
   [CHANGELOG.md](./CHANGELOG.md) describing the change. This keeps the
   changelog accurate without a separate bookkeeping pass at release time.

6. **Open a Pull Request into `main`.** Make sure CI passes. Describe what
   changed and why - for frontend changes a before/after example (source
   snippet + AST/IR shape) is helpful; for codegen changes, include the
   emulator test that exercises the new path.

7. **Squash and merge.** Keeps `main`'s history as one logical commit per
   change, regardless of how messy the branch's history was.

## Code Style

- Match the existing style in the file you're editing over any external
  convention — each part of the toolchain has its own consistent patterns.
- The frontend (`c02-frontend`) is Haskell, under `src/C02/` (`Parser/`,
  `Analyzer/`, `Lowering/`); the code generator (`c02-as`) is C and the
  disassembler (`c02-objdump`) is Rust. Follow the surrounding module's idioms
  rather than importing conventions across the language boundary.
- In `c02-as`, codegen helpers that read/write operands must go through the
  global-aware helpers (`emit_load_byte`, `emit_store_byte`, etc.) — never raw
  `zp_map_lookup`. See the `OP_EMITTER_*` and `GLOBAL_AWARE_ALU_HELPER`
  macros for the stamped-out pattern used for new addressing-mode variants.

## Reporting Bugs

Open an issue with:
- The `.c02` source that triggers the problem (minimal repro if possible)
- What you expected vs. what happened
- Output of `c02c --dump-ast` or `--dump-ir` if relevant - these make bugs
  much faster to diagnose

## Questions

If something about the architecture or grammar is unclear, open an issue -
documentation gaps are worth fixing just as much as code bugs.
