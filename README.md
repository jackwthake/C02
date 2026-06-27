<div align="center">
 
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="./docs/logo-light.png">
    <img alt="C02 Logo" src="./docs/logo-light.svg" width="300">
  </picture>

  Strongly typed, C-like systems programming language built for resource-constrained 8-bit microprocessors.
  
  [![CI](https://github.com/jackwthake/C02/actions/workflows/ci.yml/badge.svg?branch=cc02)](https://github.com/jackwthake/C02/actions/workflows/ci.yml)
</div>

## Table of Contents

- [Getting Started: Key Features & Architecture](#getting-started-key-features--architecture)
- [Current Status & Limitations](#current-status--limitations)
  - [What works today](#what-works-today)
  - [Not yet implemented](#not-yet-implemented)
- [Toolchain Usage](#toolchain-usage)
  - [Compiling the Toolchain](#compiling-the-toolchain)
  - [Running the Compiler](#running-the-compiler)
- [Language Specifications](#language-specifications)
  - [Basic Types](#basic-types)
  - [Comments](#comments)
  - [Top-Level Declarations](#top-level-declarations)
  - [Compiler Implicit Globals](#compiler-implicit-globals)
  - [Statements](#statements)
  - [Expressions](#expressions)
  - [Compilation Example](#compilation-example)
- [Binary Layout](#binary-layout)
  - [RAM](#ram)
  - [ROM](#rom)
- [Third-Party Licenses](#third-party-licenses)

## Getting Started: Key Features & Architecture

#### cc02 Compiler

1. **Source Tracking Tokenizer:** Maps characters to discrete tokens while maintaining source locations (file, line, column) for robust compilation errors.
2. **Recursive Descent Parser:** Transforms the token stream into a structured AST, treating hardware registers and standard controls as first-class grammatical constructs.
3. **Lexically Scoped Semantic Analyzer:** Two-pass validation engine over the AST. Pass 1 registers all top-level declarations (functions, structs, registers, globals) into the global symbol table. Pass 2 walks function bodies with a scoped symbol table, checking undeclared identifiers, type mismatches, argument counts/types, struct field access, lvalue validity, and return-type consistency. Invalid declarations are poisoned to prevent cascading diagnostics.
4. **IR Generator:** Lowers the analysed AST into a self-contained three-address code (TAC) intermediate representation. The IR module contains struct layouts with computed field offsets, global/register definitions with hardware addresses baked in, and one flat instruction stream per function - codegen can emit target code from the IR alone, without consulting the AST or symbol table. Supports incremental compilation: `-c` serializes the IR to a `.o` file that can be loaded back to skip the frontend entirely.
5. **Code Generator:** Emits valid 65C02 ROM binaries (32K) with a bootstrap runtime, interrupt vectors, and flat zero-page register allocation. Avoids slow stack-based execution by mapping local variables, temporaries, and parameters directly onto zero-page slots. Globals are allocated in RAM ($0200+) and initialized in the bootstrap before `JSR main`. String literals are placed in a ROM data section with backpatching fixups. Supports arithmetic (`+`, `-`, unary `-`) for all integer types (u8/i8/u16/i16), comparisons across all widths and signedness (unsigned via carry-flag, signed via N⊕V), pointer dereference, and function calls. Function calls use a fixed 2-byte-per-param ABI zone (`$EF–$FE`) for parameter passing; a callee-saves convention (PHA/PLA on all ZP slots) preserves the caller's locals across calls and enables bounded recursion. All emit paths are bounds-checked against the 32 KB ROM limit — programs that overflow produce a clear diagnostic rather than silent corruption. Compiler implicit globals (`__heap_start`, `__memory_top`) are injected automatically and initialized during bootstrap. Programs compile and run on real hardware.

#### c02-objdump Disassembler

- **Disassembler:** Decodes compiled `.bin` files back into annotated 65C02 assembly, resolving jump targets to named labels for readability. Supports section-aware output (`.text` / `.data` split), hex dumps with ASCII, and ROM usage summaries. See [c02-objdump](c02-objdump/) for more information.

## Current Status & Limitations

C02 is under active, early development. The **complete frontend** (tokenizer, parser, semantic analyzer), **IR generation**, and **code generator** are functional and tested — simple programs compile to valid 65C02 ROMs and run on real hardware.

#### What works today

- **Data movement:** variable copies, constant stores, hardware register writes. Implicit widening (u8→u16) zero-extends correctly; narrowing copies the low bytes.
- **Control flow:** `if`/`else`, `while`, `for` loops via label/jump/conditional-jump.
- **Arithmetic:** `+`, `-`, unary `-`, `*`, `/`, `%` for all integer types (u8, i8, u16, i16). Width-aware multi-byte emission for 16-bit operations with carry/borrow propagation. Multiply and divide via `__mul8`/`__div8` software subroutines.
- **Bitwise & shift ops:** `&`, `|`, `^`, `~`, `<<`, `>>` for all widths. Signed right shift uses the carry-from-sign-bit pattern for correct arithmetic extension.
- **Comparisons:** all six relational operators (`<`, `<=`, `==`, `!=`, `>=`, `>`) for all widths (u8, u16) and signedness (unsigned via carry-flag, signed via N⊕V). 16-bit comparisons use a high-byte-first pattern.
- **Increment/decrement:** `++`/`--` for both u8 and 16-bit values (pointers, u16), including globals and struct fields.
- **Pointer dereference & store:** `*p` reads via `LDA ($nn),Y`; `*p = val` writes via `STA ($nn),Y`. Both work for local and global pointer variables.
- **Pointer arithmetic:** `ptr + int` and `ptr - int` produce a pointer of the same type, enabling `*(msg + i)`-style indexed access.
- **Address-of:** `&x` resolves to the variable's ZP slot address for locals or its RAM address for globals, stored as a 16-bit pointer.
- **Type casts:** `(type)expr` — widening zero/sign-extends, narrowing copies low bytes.
- **Struct field access:** `s.field` and `ptr.field` (auto-deref) for both local and global structs. Field reads and writes work for by-value structs and pointer-to-struct, including `++field` / `--field`.
- **Global variables:** RAM-allocated globals with bootstrap initialization, correctly accessed via absolute addressing throughout all codegen paths. String literals placed in a ROM data section with backpatching fixups.
- **Compiler implicit globals:** `__heap_start` and `__memory_top` are injected automatically as `decl u16` globals and initialized during the bootstrap. `__heap_start` holds the first free RAM byte after all user globals *and* the compiler implicit globals themselves are allocated (each takes 2 bytes of RAM) — useful as a base pointer for bump allocators. `__memory_top` holds the top of the general-purpose RAM region (`$3FFF`). Both are available in any `.c02` file without a manual `decl`.
- **Function calls:** full `JSR`/`RTS` ABI with up to 8 parameters passed through the `$EF–$FE` fixed-slot ABI zone. A callee-saves convention (PHA all ZP slots on entry, PLA in reverse on return) preserves caller locals across calls. Bounded recursion is supported — stack depth is limited to ≈256 / (function ZP byte count).

#### Not yet implemented

- **String local variables** — `u8 *msg = "..."` only works at file scope; local string pointer initialization is not yet supported.
- **Arrays** — no array type or subscript syntax (`a[i]`). Use pointer arithmetic (`*(ptr + i)`) in the meantime.
- **`break` / `continue`** — not yet supported inside loops.
- **Missing-return detection is shallow.** A non-void function with no `return` at the end is flagged, but the analyzer does not perform full path-coverage analysis.

If you're exploring the codebase: the parser ([parser.c](cc02/src/parser/parser.c)), the analyzer ([analyzer.c](cc02/src/analysis/analyzer.c)), the IR generator ([ir.c](cc02/src/ir-gen/ir.c)), and the code generator ([generator.c](cc02/src/code-gen/generator.c)) are the main files. Issues and PRs are welcome.

## Toolchain Usage

### Compiling the Toolchain

```shell
sudo apt install build-essential curl python3 python3-pip -y

# Official Rust install script (for c02-objdump)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# py65 6502 emulator (for runtime tests)
pip install py65

git clone https://github.com/jackwthake/C02.git
cd C02
make
```

### Running the Compiler

```bash
cc02 [OPTIONS] <FILE>
```

#### Options

- `<FILE>`:               Input file (`.c02` source or `.o`/`.out` IR object)
- `-h, --help`:           Show help message
- `-c`:                   Incremental compile - emit a `.o` IR object file instead of a final binary
- `-o, --output`:         Specify output file
- `--token-dump`:         Dump the token list after tokenization
- `--ast-dump`:           Dump the AST after parsing
- `--symbol-dump`:        Dump the global symbol table after analysis
- `--ir-dump`:            Dump the IR (TAC instructions) after lowering
- `--syntax-check-only`:  Stop after syntax and semantic checks
- `--time-report`:        Print a report showing how long each stage of compilation took

**Incremental compilation:**

```bash
cc02 -c hello_world.c02 -o hello_world.o   # compile to IR object
cc02 --ir-dump hello_world.o                # inspect the IR from the object file
```

### Pretty Error Messages

![Pretty error reporting](./docs/pretty-errors.png)

All generated error messages are presented in a clang like format with concise source locations. The printed file locations use an editor-friendly format, enabling you to click to open the affected file.

---

## Language Specifications

> The grammar below reflects what the tokenizer and parser currently accept. Semantic analysis validates the full AST after parsing, IR generation lowers it to TAC, and the code generator emits 65C02 machine code — see [Getting Started](#getting-started-key-features--architecture) and [Current Status](#current-status--limitations) for what's working today.

### Basic Types

- `u8` / `i8`: 8-bit integers (unsigned / signed)
- `u16` / `i16`: 16-bit integers (unsigned / signed)
- `void`: Function return types with no payload.
- `struct` names: a bare identifier in type position resolves to a struct type (e.g. `Point p;`).
- Pointer types: any base type followed by one or more `*` (e.g. `u8 *msg`, `u16 **pp`).

### Comments

```c
// single-line comment

/*
  block comment
*/
```

### Top-Level Declarations

A `.c02` file is a sequence of top-level declarations: functions, `reg` declarations, `struct` declarations, global variables, and forward declarations (`decl`).

#### Functions

```c
fn name(u8 a, u16 *b) -> void {
  // body
}
```

- Parameter list is `(type name, type name, ...)`, can be empty: `()`.
- Return type is required, introduced with `->`.

#### Registers (`reg`)

Hardware interface registers are pinned directly to absolute memory addresses.

```c
reg u8 PORTA @ 0x6001;
reg u8 PORTB @ 0x6000;
```

#### Structs

```c
struct Point {
  u8 x;
  u8 y;
}
```

- Body is a sequence of `type name;` fields, no nested initialisers.
- A trailing `;` after the closing `}` is optional.

#### Global Variables

```c
u8 *msg = "Hello C02!";
u16 counter;
Point origin;
```

- Same form as a local variable declaration: `type name;` or `type name = expr;`.
- Struct-typed globals are supported (`Point p;`).

#### Forward Declarations (`decl`)

Forward declarations introduce the signature of a function or global defined in another translation unit, allowing cross-file references with incremental compilation (`-c`).

```c
decl fn send_byte(u8 b) -> void;
decl u8 counter;
```

- A `decl` for a function uses the same signature syntax as `fn` but has no body.
- A `decl` for a global is `decl type name;` with no initialiser.
- Redeclaring a name that already exists in the same file is an error.

##### Compiler Implicit Globals

The compiler automatically injects a small set of `u16` globals that expose runtime memory layout information. No `decl` is needed — they are available in every translation unit.

| Name | Value | Description |
| :--- | :--- | :--- |
| `__heap_start` | first free RAM address after all globals | Base pointer for simple bump allocators. |
| `__memory_top` | `$3FFF` | Top of the general-purpose RAM region. |

### Statements

```c
// variable declaration (local)
u8 x = 5;

Point p;                      // struct-typed declaration
p = Point{ .x = x, .y = 10 }; // struct with initializer
p = Point{};                  // zero initialized struct

Point *p2; // or p2 = null;      pointer to a Point struct, uninitialized
Point *p2 = &p;               // pointer to a Point struct, initialized

// assignment (also: += -= *= /= %=)
x = x + 1;
x += 1;

// return
return;
return x;

// if / else if / else
if (x > 0) {
  // ...
} else if (true) { // `true` and `false` are accepted keywords
  // ...
} else {
  // ...
}

// while
while (x < 10) {
  x += 1;
}

// for (any of the three clauses may be empty)
for (u8 i = 0; i < 10; i += 1) {
  // ...
}

// function call statement
do_thing(a, b);
```

### Expressions

Precedence, lowest to highest:

```
||  &&  |  ^  &  ==  !=  <  >  <=  >=  <<  >>  +  -  *  /  %  (unary)  (postfix)
```

- **Unary (prefix):** `!` (logical not), `-` (negate), `&` (address-of), `~` (bitwise not), `++` / `--`, `*` and `@` (dereference).
- **Postfix:** `.field` field access, chainable (`a.b.c`). Auto-dereferences struct pointers (`ptr.field` where `ptr` is a `Struct*`).
- **Calls:** `name(arg1, arg2, ...)`.
- **Casts:** `(type)expr`, e.g. `(u16)x`.
- **Grouping:** `(expr)`.
- **Literals:** decimal/hex integers (`l_num`), string literals (`l_string`), identifiers.

### Compilation Example

This program cycles LEDs connected to PORTB on a 65C02 breadboard — counting up from 0 to 255 and back down in an infinite loop. It compiles to a valid 32K ROM and runs on real hardware.

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
cc02 led_counter.c02 -o led_counter.bin   # compile to 32K ROM
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

### Third-Party Licenses

| Dependency | License | Used By |
| :--- | :--- | :--- |
| [clap](https://github.com/clap-rs/clap) | MIT / Apache-2.0 | `c02-objdump` CLI argument parsing |
| [py65](https://github.com/mnaberez/py65) | BSD | Test harness 65C02 emulator for runtime verification |
