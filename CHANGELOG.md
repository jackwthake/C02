# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/) - while
the project is in `0.x`, breaking changes may land in MINOR releases; PATCH
releases are reserved for bug fixes only.

## [Unreleased]

### Changed

- Generalized the arena allocator out of `parser.c` into shared
  infrastructure, so semantic analysis can reuse it for its own
  allocations.

## [0.1.1] - 2026-06-20

No code changes - this release formalizes the project's licensing and
contribution process.

### Added

- `LICENSE`: GPLv3, with a compiler-output exception so programs compiled
  *with* C02 are not themselves subject to GPL terms.
- `CONTRIBUTING.md` documenting the branch naming, PR, and changelog
  conventions for the project.
- "Current Status & Limitations" section in the README, consolidating what's
  implemented vs. not in one place.
- Third-party license attribution for `c02-objdump`'s `clap` dependency
  (dual-licensed MIT/Apache-2.0).

## [0.1.0] - 2026-06-20

Initial public release. **Frontend only** - tokenizer, parser, and AST
printer. No semantic analysis or code generation yet; see the README's
"Current Status & Limitations" section for the full list of what's not
implemented.

### Added

- Source-tracking tokenizer with file/line/column on every token.
- Recursive descent parser covering:
  - Full expression precedence chain: `||  &&  |  ^  &  ==  !=  <  >  <=  >=  <<  >>  +  -  *  /  %`, plus unary `! - & ~ ++ -- * @`.
  - Statements: variable declarations, assignment (including compound `+= -= *= /= %=`) to any lvalue (identifier, field access, or dereference), `return`, `if` / `else if` / `else`, `while`, `for` (with optional clauses), function calls.
  - Top-level declarations: `fn`, `reg` (hardware register pinned to an absolute address), `struct`, and global variables.
  - Structs: field declarations, chained field access (`a.b.c`), and designated-initializer struct literals (`Point { .x = 0, .y = 0 }`).
  - C-style casts (`(u16)x`) and grouped expressions.
- Arena allocator for AST nodes, with growable scratch buffers for variable-length lists (statements, params, struct fields, call args) committed into the arena on completion.
- Clang-style error reporting: colorized output, caret-span source highlighting, and "expected / context" diagnostics for every parse error.
- AST printer (`--ast-dump`) producing a readable tree view of any parsed program.
- Golden-file test suite (Python harness) covering bitwise ops, conditionals, global vars, registers, and struct declarations.
- `--token-dump`, `--time-report`, and `--syntax-check-only` CLI flags.
- `c02-objdump`, a companion disassembler for decoding compiled `.bin` files back into annotated 65C02 assembly.

### Known Limitations

- No semantic analysis: types, struct fields, and symbol references are not validated.
- No code generation: `cc02` does not yet produce a working 65C02 binary.
- No array type or subscript syntax.
- `->` is not implemented; field access through a pointer is intended to auto-dereference via semantic analysis once that exists.