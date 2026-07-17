# Linker tests for `c02-ld`

These tests exercise the **linker** — the stage that merges several IR object
files into one and resolves the symbol namespace (`c02-ld/bin/link.ml`).
Where `test/emu` pins what a single compiled program *does*, these pin what
happens when several translation units are brought together: cross-module
references resolve, duplicate definitions are rejected, identical registers and
structs are de-duplicated, and a partial link keeps unresolved symbols for a
later stage.

Every case is driven through the toolchain driver `bin/c02c`, exactly as a user
would link multiple sources — `c02-frontend` compiles each `.c02` to an object,
then `c02-ld` merges them (`-c` stops there; without it, `c02-as` finishes the
ROM). Positive cases that produce a whole program are then *run* in the same
py65 emulator the `test/emu` suite uses, so a successful link is proven by
executing the linked ROM, not just by exit code.

## Running

```sh
python3 test/linker/run.py                 # run every case
python3 test/linker/run.py reg_dedup       # only the named case(s)
python3 test/linker/run.py -v              # show every assertion, not just failures
make linker-test                           # build the toolchain, then run
```

Requires `py65` (`pip install py65`), same as the emulator suite.

## Layout

Each subdirectory of `tests/` is one case:

```
tests/<case>/
  *.c02     one or more module sources (separate translation units)
  spec      directives (below)
```

Modules reference each other with the `decl` forward-declaration keyword
(SPEC §4.6) — `decl fn f(...) -> T;` for a function, `decl T name;` for a
variable — which the frontend emits as an *extern* for the linker to resolve.

## Spec directives

`spec` is a plain `KEY: value` file (`#` comments allowed):

| Directive | Meaning |
|---|---|
| `DESC: <text>` | human description (required) |
| `MODE: run \| link \| reject` | what the case checks (required) |
| `FILES: a.c02 b.c02` | link order (optional; default: sorted `*.c02`) |
| `PARTIAL: yes` | link with `-c` — partial link, no codegen, no `main` required |

The assertions depend on `MODE`:

**`MODE: run`** — link, generate a ROM, run it, check final machine state. Uses
the same target/operator vocabulary as `test/emu` (see that README):

```
EXPECT: mem8[0x6000] == 42
```

**`MODE: reject`** — linking *must* fail; the case asserts the diagnostic. The
link is run with `-c` so the failure is isolated to the linker:

```
STDERR: Redefined function: foo      # substring of c02-ld's stderr
```

**`MODE: link`** — linking must succeed; assert on the merged IR
(`--dump-ir`). Used where runtime is blind to the property (a de-dup collapse,
an extern being dropped or kept):

```
IR_COUNT: struct Pt == 1       # count of matching lines
IR_PRESENT: Externs            # substring must appear
IR_ABSENT: Externs             # substring must NOT appear
```

A `link` case with no `IR_*` directive just asserts the link succeeded (e.g.
`library_no_main`).

## Coverage

The cases map onto the linker's resolution rules (`combine` and `dedup_structs`
in `link.ml`):

- **Resolution across modules** (`run`): a `decl`'d function and a `decl`'d
  global are matched to their definitions and the program runs; a tentative
  global merges with an initialized one.
- **De-duplication** (`link`): an identical register or struct declared in two
  modules collapses to one; a resolved extern is dropped; an unresolved extern
  survives a partial link; a `main`-less module links as a library.
- **Rejection** (`reject`): one case per `failwith` site — duplicate function,
  conflicting externs, extern/definition signature or type mismatch, function
  vs. variable and variable vs. register kind clashes, conflicting registers,
  multiple global initializers, conflicting global types, cross-kind namespace
  collision, and struct redeclaration.

## Writing a new case

1. `mkdir tests/<case>` and add the module `.c02` files.
2. Add a `spec` with `DESC`, `MODE`, and the matching assertions.
3. For `run` cases, report results by writing to `$6000` and keep the program
   terminating (no unbroken loop). For `reject`, use a `STDERR` substring that
   is distinctive to the linker so a frontend error can't satisfy it.
4. `python3 test/linker/run.py <case>` to check it.
