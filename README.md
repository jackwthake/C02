<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="./docs/logo-light.png">
    <img alt="C02 Logo" src="./docs/logo-light.svg" width="300">
  </picture>

  Strongly typed, C-like systems programming language built for resource-constrained 8-bit microprocessors.
  
  [![CI](https://github.com/jackwthake/C02/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/jackwthake/C02/actions/workflows/ci.yml)
</div>

## Table of Contents

- [Getting Started: Key Features & Architecture](#getting-started-key-features--architecture)
- [Current Status & Limitations](#current-status--limitations)
- [Toolchain Usage](#toolchain-usage)
  - [Compiling the Toolchain](#compiling-the-toolchain)
  - [Running the Compiler](#running-the-compiler)
- [Language Specifications](#language-specifications)
- [Binary Layout](#binary-layout)
  - [RAM](#ram)
  - [ROM](#rom)
- [Third-Party Licenses](#third-party-licenses)

## Getting Started: Key Features & Architecture

C02 compiles through a small pipeline of standalone tools, wired together by the `c02c` driver:

1. **`c02-frontend`** (Haskell) — tokenizer, recursive-descent parser, lexically-scoped semantic analyzer, and IR generator. Lowers `.c02` source into a self-contained three-address-code (TAC) intermediate representation — struct layouts with computed field offsets, globals/registers with hardware addresses baked in, and one flat instruction stream per function — then serializes it to a `.o` object, one per translation unit.
2. **`c02-ld`** (OCaml) — the linker. Merges one or more `.o` objects into a single relocatable object and resolves the symbol namespace: it binds cross-module references (a `decl` forward declaration in one file to its definition in another), de-duplicates identical `reg` and `struct` declarations, applies the tentative-definition rule to globals, and rejects genuine conflicts (duplicate definitions, signature/type mismatches, namespace collisions). It never requires a `main`, so a merged object may be a library. Incremental compilation (`-c`) stops here, emitting the merged object.
3. **`c02-as`** (C) — the code generator. Consumes the linked `.o` IR object and emits a 32 KB 65C02 ROM: bootstrap runtime, interrupt vectors, and flat zero-page allocation of locals, temporaries, and parameters (no slow stack-machine execution). Globals live in RAM (`$0200+`) and are initialized before `JSR main`; string literals go in a ROM data section with backpatch fixups. Every emit path is bounds-checked against the 32 KB limit, so overflow is a clear diagnostic rather than silent corruption.
4. **`c02c`** — the driver that runs the frontend on each source, then `c02-ld`, then `c02-as`, managing intermediate `.o` files. This is the command you normally invoke.
5. **`c02-objdump`** (Rust) — disassembler. Decodes a compiled `.bin` back into annotated 65C02 assembly, resolving jump targets to named labels, with section-aware output (`.text` / `.data`), hex dumps, and ROM usage summaries. See [c02-objdump](c02-objdump/).

The function-call ABI passes up to 8 parameters through a fixed 2-byte-per-param zero-page zone (`$EF–$FE`); a callee-saves convention (PHA/PLA over the ZP slots) preserves caller locals across calls and enables bounded recursion. Compiler implicit globals (`__heap_start`, `__memory_top`) are injected automatically.

## Current Status & Limitations

The language is broadly in place: data movement and hardware-register I/O; `if`/`else`, `while`, `for`, `break`/`continue`; arithmetic, bitwise, shift, and comparison operators across `u8`/`i8`/`u16`/`i16` (multiply/divide via `__mul8`/`__div8`/`__mul16`/`__div16` software routines); pointers (`&`, `*`, `ptr ± int`), type casts, structs and field access, globals, string literals, and function calls with recursion. [`docs/SPEC.md`](docs/SPEC.md) is the **normative** definition of the language and its exact semantics; [`docs/DEVIATIONS_hs_impl.md`](docs/DEVIATIONS_hs_impl.md) records where this implementation currently diverges from it (it is generally *stricter* than the original).

Not yet implemented: **arrays** (use pointer arithmetic, `*(ptr + i)`, in the meantime) and **compound bitwise/shift assignment** (`&=`, `|=`, `^=`, `<<=`, `>>=` — the arithmetic forms `+= -= *= /= %=` work).

If you're exploring the codebase, the stages live in [`c02-frontend/`](c02-frontend/) (Haskell), [`c02-ld/`](c02-ld/) (OCaml, the linker), and [`c02-as/`](c02-as/) (C, the code generator). Issues and PRs are welcome.

## Toolchain Usage

### Compiling the Toolchain

The four stages need a C compiler (`c02-as`), GHC + Cabal (the Haskell `c02-frontend`), OCaml + dune (the `c02-ld` linker), and Rust (`c02-objdump`); Python 3 drives `c02c` and the tests.

```shell
sudo apt install build-essential curl python3 python3-pip -y

# GHC + Cabal for the Haskell frontend (ghcup is the usual installer)
curl --proto '=https' --tlsv1.2 -sSf https://get-ghcup.haskell.org | sh

# OCaml + dune for the linker (c02-ld); opam ships the compiler
sudo apt install opam -y
opam init -y && eval $(opam env)
opam install dune -y

# Rust, for c02-objdump
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# py65 6502 emulator, for the runtime tests
pip install py65

git clone https://github.com/jackwthake/C02.git
cd C02
cabal update   # first build only, to populate the Cabal package index
make
```

### Running the Compiler

`c02c` is the driver; it runs the frontend and the code generator for you.

```bash
c02c [OPTIONS] <FILE>...
```

#### Options

- `<FILE>`:            Input file(s) — `.c02` source and/or `.o` IR objects
- `-h, --help`:        Show help message
- `-o, --output`:      Output path (default `a.out`)
- `-c`:                Compile and link to a relocatable `.o` object (a partial link — no `main` required) instead of a full ROM
- `--parse-only`:      Check syntax only, produce no output
- `--dump-ast`:        Print the AST after parsing (no output file)
- `--dump-ir`:         Print the IR (TAC) the code generator consumes (no output file)
- `--strip-debug`:     Omit the `C02S` symbol table from the final ROM

**Separate compilation and linking:**

Pass several sources and/or `.o` objects and `c02c` compiles each source, then links everything into one output. A `.c02` references a symbol defined in another file with a `decl` forward declaration (`decl fn f(u8 x) -> void;`, `decl u8 counter;`).

```bash
c02c -c util.c02 -o util.o             # compile + partial-link to a relocatable object
c02c main.c02 util.o -o program.bin    # link that object into a full ROM
c02c main.c02 util.c02 -o program.bin  # or compile and link several sources at once
c02c --dump-ir main.c02 util.c02       # inspect the linked IR before codegen
```

---

## Language Specifications

The full grammar, type system, and exact runtime semantics live in [`docs/SPEC.md`](docs/SPEC.md) — the normative reference. The program below is a representative taste: `reg` declarations pin hardware ports to absolute addresses, and `fn main() -> void` is the entry point.

It cycles LEDs connected to PORTB on a 65C02 breadboard — counting up from 0 to 255 and back down in an infinite loop — and compiles to a valid 32 KB ROM.

```c
reg u8 PORTB @ 0x6000;
reg u8 DDRB @ 0x6002;

fn main() -> void {
  DDRB = 0xFF; // Set all pins of PORTB as output

  while(true) {
    u8 i = 0;
    for (; i < 255; ++i) {
      PORTB = i;
    }

    PORTB = i;

    for (; i > 0; --i) {
      PORTB = i;
    }
  }
}
```

```bash
c02c led_counter.c02 -o led_counter.bin   # compile to 32K ROM
c02-objdump led_counter.bin               # disassemble to inspect the output
```

---

### Binary Layout

Every compiled binary is a flat **32 KB ROM image** (`$8000–$FFFF`) loaded at a fixed base address. The layout is always the same regardless of program size — unused space is filled with `$EA` (NOP). See [memmap.md](./docs/memmap.md) for more info on memory boundaries.

#### RAM

```
$0000 ┬─────────────────────────────────────────────
      │  Zero Page  (see ZP table below)
$0100 ├─────────────────────────────────────────────
      │  Hardware stack  (6502 fixed; $01FF = top)
$0200 ├─────────────────────────────────────────────
      │  User globals  (RAM_START; allocated upward by allocate_globals)
      ├╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌
      │  __heap_start  (u16, 2 bytes — compiler implicit)
      │  __memory_top  (u16, 2 bytes — compiler implicit)
      ├───────────────────────────────────────────── ← __heap_start value (first free byte)
      │  (free for heap / dynamic use)
$3FFF ┴───────────────────────────────────────────── ← __memory_top value
```

#### Zero-Page Hardware-Register Layout

To maximize compilation density and execution speed, the code generator reserves and maps lower RAM (`$0000–$00FF`, **The Zero Page**) to form a virtual register file:

| Address Range | Identifier | Purpose |
| :--- | :--- | :--- |
| **`$00`** | `FP` | **Frame Pointer:** Initialized to `$01FF` at startup. |
| **`$02–$03`** | `RET` | **Return Register:** Holds function return values (u8 in `$02`, u16 in `$02:$03`). |
| **`$04–$DF`** | `r0`–`r219` | **Scratch Registers:** Compiler-managed temporaries, locals, and globals. Allocated per-function from `$04` upward, striding by type size (1 byte for u8/i8, 2 for u16/i16/pointers). |
| **`$E0–$E7`** | — | **16-bit Arithmetic Helper Zone:** Fixed slots for `__mul16`, `__div16`, `__sdiv16` helpers. `$E0:$E1` = arg1, `$E2:$E3` = arg2, `$E4:$E5` = result, `$E6:$E7` = remainder. |
| **`$E8–$EC`** | — | **8-bit Arithmetic Helper Zone:** Fixed slots for `__mul8`, `__div8`, `__sdiv8` helpers. `$E8` = arg1, `$E9` = arg2, `$EA` = result, `$EB` = remainder, `$EC` = sign flags (bit 7 = negate quotient, bit 6 = negate remainder). |
| **`$ED–$EE`** | — | Reserved for future helpers. |
| **`$EF–$FF`** | `a0`–`a7` | **Function ABI Zone:** Fixed 2-byte slots for parameter passing. Caller populates before `JSR`; callee reads at entry. Supports up to 8 sixteen-bit parameters. |

#### ROM

```
$8000 ┬───────────────────────────────────────────── ← Reset vector target
      │  Bootstrap  (SEI · CLD · stack init · global init · JSR main · halt)
      ├─────────────────────────────────────────────
      │  .text  — function bodies  (main first, then callees, then helpers)
      ├───────────────────────────────────────────── ← code/data boundary marker ($FFF8–$FFF9)
      │  .data  — null-terminated string literals
      ├─────────────────────────────────────────────
      │  C02S symbol table  (if --strip-debug not set)
      |    magic "C02S" · u16 count · [u16 addr · name\0] …
      ├─────────────────────────────────────────────
      │  NOP fill  ($EA bytes)
$FFF6 ├─────────────────────────────────────────────
      │  Symbol table pointer  (LE u16; $EAEA = absent)
$FFF8 ├─────────────────────────────────────────────
      │  Code/data boundary marker (LE u16; first NOP-fill byte)
$FFFA ├─────────────────────────────────────────────
      │  NMI vector   (LE u16)
$FFFC ├─────────────────────────────────────────────
      │  Reset vector (LE u16; always $8000)
$FFFE ├─────────────────────────────────────────────
      │  IRQ vector   (LE u16)
$FFFF ┴─────────────────────────────────────────────
```

The `$FFF8–$FFF9` boundary word and the `$FFF6–$FFF7` symbol-table pointer are read by `c02-objdump` to locate the `.text`/`.data` split and resolve function names. Older binaries that predate these fields have `$EAEA` at `$FFF6` and are disassembled with auto-generated `L0`/`L1`/… labels as a fallback.

---

### References

1. [Crafting Interpreters](https://craftinginterpreters.com/contents.html) — the primary reference used throughout development.
2. [rui314/chibicc](https://github.com/rui314/chibicc) — structurally similar (recursive descent, etc.), found after starting this project; not directly followed, but worth a look.

---

### Third-Party Licenses

| Dependency | License | Used By |
| :--- | :--- | :--- |
| [clap](https://github.com/clap-rs/clap) | MIT / Apache-2.0 | `c02-objdump` CLI argument parsing |
| [megaparsec](https://github.com/mrkkrp/megaparsec) | BSD-2-Clause | `c02-frontend` lexer/parser combinators |
| [py65](https://github.com/mnaberez/py65) | BSD | Test harness 65C02 emulator for runtime verification |
