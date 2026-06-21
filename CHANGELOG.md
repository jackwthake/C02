# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/) - while
the project is in `0.x`, breaking changes may land in MINOR releases; PATCH
releases are reserved for bug fixes only.

## [Unreleased]

## [0.2.0] - 2026-06-21

### Added

- **Semantic analysis** — two-pass analyzer that validates the full AST:
  - Pass 1: registers all top-level declarations (functions, structs,
    registers, global variables) into the global symbol table with
    redeclaration checking.
  - Pass 2: recursive walk of function bodies with scoped symbol tables,
    checking for undeclared identifiers, type mismatches, wrong argument
    counts/types, unknown struct fields, and redeclarations.
  - `resolve_expr_type()` for full expression type resolution: literals
    (with smallest-fitting integer type), identifiers, function calls,
    binary/unary operators, dereferences, address-of, casts, field access,
    and struct initializers.
  - Integer widening: `u8` → `u16` is allowed implicitly; narrowing
    requires an explicit cast.
  - `null`/`0` is compatible with both pointer and integer types.
  - For-loops get their own scope so loop variables don't leak.
  - `main` function existence check after pass 1.
- `analyzer_print.c` and `--symbol-dump` CLI flag for printing the
  global symbol table after analysis.
- `analyzer_t` struct owning its own arena (mirroring `parser_t`), with
  `analyzer_init()`/`analyzer_free()` lifecycle.
- Smoke tests for the analyzer's scope stack, symbol insertion, lookup,
  and shadowing behavior.
- Compiler test cases covering analyzer error paths (undeclared
  identifiers, type mismatches, call errors, struct errors) and
  success paths (scoping, widening, pointers, basic analysis).
- **Analyzer hardening** — additional validation closing gaps where invalid
  programs were previously accepted silently:
  - Named struct types are checked for existence in *every* declaration form
    (locals, parameters, globals, registers, struct fields, function return
    types), not just struct initializers and field access. A symbol whose
    declared type is invalid is poisoned, so later uses don't cascade
    duplicate diagnostics.
  - A non-pointer `void` is rejected as a variable/parameter/field/global
    type (`void*` remains valid as a null pointer).
  - Integer literals that don't fit any supported type are now an error
    (previously swallowed); the lexer rejects literals that overflow `long`.
    Negative literals are typed from their value (`-5` is `i8`, `-300` is
    `i16`).
  - Assignment targets and the operands of `&`, `++`, and `--` must be
    lvalues.
  - Global variable initializers are type-checked (resolved in the global
    scope), just like local declarations - previously they were ignored.
  - A bare `return;` in a non-void function is rejected, and a non-void
    function that can fall off its end without returning a value is flagged.
    (This last check is shallow: a function ending in a control-flow statement
    — e.g. a one-armed `if` that falls through, or an `if`/`else` where only
    some branches return — is assumed to return and is not flagged. Full
    path-coverage analysis is future work.)
- Struct-typed globals (`Point p;` at file scope) now parse, matching the
  form used for locals.
- Field access auto-dereferences a single-level struct pointer (`c.field` on
  a `Struct*`), since there is no `->` operator.
- A negative-test corpus exercising each new check, plus positive tests for
  pointer field auto-deref and negative-literal typing.

### Changed

- Renamed `scope_stack_t` → `analyzer_t` and all associated functions
  to `analyzer_*` prefix for consistency with `parser_t`.
- Consolidated cleanup in `main.c` into a single `goto finish` exit path,
  eliminating repeated free-lists at each error point.
- Test harness now routes `--ast-dump` to parser tests and `--symbol-dump`
  to analyzer tests based on filename prefix.
- Refactored program error codes so each failure stage gets its own exit code.
- Generalized error handling and printing for parsing onwards
  (including sem. analysis). Moved source location tracking to a
  dedicated type, `token_location_t` to support pretty error messages
  after tokens aren't directly accessible.
- Updated test harness to run smoke binaries and check for memory leaks.
- Generalized the arena allocator out of `parser.c` into shared
  infrastructure, so semantic analysis can reuse it for its own
  allocations.
- Diagnostics now print the full pointer depth (`u16**`, not `u16*`) and name
  the actual type in field-access-on-non-struct errors; a malformed number
  literal and a repeated unknown-type are each reported once rather than per
  character / per use.

### Fixed

- Fixed a segfault when a parse error's offending token was a numeric literal.
- Fixed a memory leak in the arena allocator when a standard chunk allocation
  followed an oversized one.
- String literals with escape sequences no longer drop a trailing character
  per escape, and common escapes (`\n`, `\t`, `\\`, `\"`, ...) are decoded.

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

- No code generation: `cc02` does not yet produce a working 65C02 binary.
- No array type or subscript syntax.
- `->` is not implemented; field access through a pointer is intended to auto-dereference via semantic analysis once that exists.
