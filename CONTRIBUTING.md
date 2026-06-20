# Contributing to C02

Thanks for taking a look at C02! The project is in early, active development -
the workflow below is intentionally lightweight, but consistent.

## Project Status

C02 is currently a **parser frontend only** - see the [README's Current
Status & Limitations](./README.md#current-status--limitations) section
before diving in. Semantic analysis and code generation are the next major
areas of work, so expect the AST and grammar to keep shifting for a while.

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
   | `feat/`     | New language features, parser additions   |
   | `fix/`      | Bug fixes                                  |
   | `refactor/` | Internal restructuring, no behavior change |
   | `docs/`     | README, comments, this file, etc.          |
   | `test/`     | Test additions or test harness changes     |

   ```bash
   git checkout -b feat/array-types
   ```

3. **Commit as you go.** Work-in-progress commits on your branch don't need
   to be clean - history gets squashed on merge (see below).

4. **Add tests.** New parser features should come with a golden-file test
   under `cc02/tests/` (see existing `parser_*.c02` / `.golden` pairs for the
   pattern). Bug fixes should add a regression case where practical.

5. **Update the changelog.** Add a line under `[Unreleased]` in
   [CHANGELOG.md](./CHANGELOG.md) describing the change. This keeps the
   changelog accurate without a separate bookkeeping pass at release time.

6. **Open a Pull Request into `main`.** Make sure CI passes. Describe what
   changed and why - for grammar/parser changes, a small before/after
   example (source snippet + relevant AST shape) is the most useful thing
   you can include.

7. **Squash and merge.** Keeps `main`'s history as one logical commit per
   change, regardless of how messy the branch's history was.

## Code Style

- Match the existing style in the file you're editing over any external
  convention - this codebase has its own consistent patterns (see the header
  comment in `parser.c` for the parser's design rationale).
- Prefer the existing macro patterns (`BINOP_LEVEL`, `DEFINE_NODE_SCRATCH`,
  `DEFINE_VALUE_SCRATCH` in `parser_macros.inc`) over hand-rolled duplicates
  when adding something that fits an existing shape.
- Parser errors should go through `GENERATE_ERROR`/`EXPECT_SYMBOL` and
  propagate via `GUARD(p)`, consistent with everything else in `parser.c`.

## Reporting Bugs

Open an issue with:
- The `.c02` source that triggers the problem (minimal repro if possible)
- What you expected vs. what happened
- Output of `--ast-dump` or `--token-dump` if relevant - these make parser
  bugs much faster to diagnose

## Questions

If something about the architecture or grammar is unclear, open an issue -
documentation gaps are worth fixing just as much as code bugs.
