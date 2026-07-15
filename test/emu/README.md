# Emulator tests for the code generator (`c02-as`)

These are **runtime** tests: each `.c02` program is compiled all the way to a
32 KB 6502 ROM, loaded into a [py65](https://github.com/mnaberez/py65) 65C02
emulator, run from the reset vector until the generated halt loop, and then
checked against expectations. Where the golden tests under `test/parser` and
`test/analyzer` pin the *frontend's* text output, these pin what the machine
code actually **does**.

The compile path is the full driver (`bin/c02c` = `c02-frontend` → `c02-as`),
so a program must satisfy the (strict) frontend before it can exercise codegen.

## Running

```sh
test/emu/run.py                 # build via make, then run everything
test/emu/run.py add mul         # only tests whose filename contains a filter
test/emu/run.py --no-build      # use the current bin/ as-is
test/emu/run.py -v              # show every assertion, not just failures
test/emu/run.py --ir-on-fail    # on failure, dump the IR c02-as consumed
```

Requires `py65` (`pip install py65`).

`--ir-on-fail` is the diagnosis lever: because tests run through the frontend
too, a failure could be a lowering bug or a codegen bug. It re-emits the `.o`
directly and prints the IR `c02-as` actually saw — if the frontend rejected the
program, it says so and codegen never ran.

## How a test observes results

`main` must be `fn main() -> void` (SPEC §7.4), so programs can't just return a
value. Instead they report the way real hardware would: by writing to a
memory-mapped `reg`. The convention here mirrors the Ben Eater VIA (see
`docs/memmap.md`):

```c
reg u8  PORTB @ 0x6000;   // 8-bit results
reg u16 PORT16 @ 0x6000;  // 16-bit results (little-endian at $6000/$6001)
```

The harness reads those addresses back out of emulator memory after the halt.

## Expectation directives

Directives live in `//` comments anywhere in the file:

| Directive | Meaning |
|---|---|
| `// EXPECT: <target> == <value>` | assertion checked after the run |
| `// CYCLES: <n>` | max instructions to execute (default 100 000) |
| `// DESC: <text>` | human description (defaults to the first comment) |

`<target>` is one of:

| Target | Reads |
|---|---|
| `mem8[A]` / `mem16[A]` | a byte / LE word of final RAM/IO state |
| `rom8[A]` / `rom16[A]` | a byte / LE word of the ROM image (structural; no run) |
| `rom_size` | ROM file size in bytes |
| `sp` / `a` / `x` / `y` | final CPU register |
| `trace8[A]` | ordered list of every byte written to `A` during the run |

`<value>` is a decimal or `0x` integer, a `'c'` char, a `[list, ...]`, or a
`"string"` (for `trace8`). Comparison ops: `== != < > <= >=`; `trace8` also
accepts `endswith` (handy for streamed output — see `string_walk.c02`).

Example:

```c
// DESC: u16 300 + 200 == 500
// EXPECT: mem16[0x6000] == 500
reg u16 PORT16 @ 0x6000;
fn main() -> void {
  u16 a = 300;
  u16 b = 200;
  PORT16 = a + b;
}
```

## Coverage

Programs walk the TAC opcode set (`c02-as/src/ir.h`) and the arithmetic helper
routines, with the bug-prone signed/16-bit paths hit hardest: `__mul8/16`,
`__div8/16`, `__sdiv8/16` with negative operands, casts across width and sign,
the callee-saves ABI (clobber, recursion, the 8-parameter boundary), pointers
and `&`/`*`, struct field load/store, and interrupt-vector wiring. Expected
values are taken from the SPEC (notably §3–§6 and Appendix B "Confirmed Runtime
Semantics"); a handful of programs adapt the upstream `cc02` emulator suite that
Appendix B cites as its oracle.

## Writing a new test

1. Add `foo.c02` here. Report results by writing to `$6000`.
2. Add `// EXPECT:` line(s). Keep it terminating — no unbroken `while (true)`.
3. Remember the frontend is stricter than C: a bare positive literal is `u8`,
   so signed values need a negative literal (`i8 a = -5;`) or a cast
   (`(i8)3`).
4. `test/emu/run.py foo` to check it.
