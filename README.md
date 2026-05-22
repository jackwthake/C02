# C02 Compiler

[![CI](https://github.com/jackwthake/C02/actions/workflows/ci.yml/badge.svg)](https://github.com/jackwthake/C02/actions/workflows/ci.yml)

A systems language compiler for the C02 language, implemented in Rust. It reads `.c02` source files, tokenizes and parses them into an Abstract Syntax Tree (AST), runs an advanced semantic analysis pass, and generates native, high-performance assembly targeted for the *VASM 65C02 Oldstyle Assembler*.

## What it is

C02 is a strongly typed, C-like systems programming language designed specifically for resource-constrained 8-bit microprocessors (65C02). It eschews heavy runtimes or interpreted VMs, compiling directly down to tight bare-metal machine instructions.

## Key Features & Architecture

The compiler is built as a complete, modern multi-stage pipeline:

1. **Source Tracking Tokenizer:** Maps characters to discrete tokens while maintaining source locations (file, line, column) for robust compilation errors.
2. **Recursive Descent Parser:** Transforms the token stream into a structured AST, treating hardware registers and standard controls as first-class grammatical constructs.
3. **Lexically Scoped Semantic Analyzer:** Implements a type synthesizer and validation engine. It enforces a hierarchical symbol table structure to handle block scoping (`if/else`, `while`), tracking variable lifetimes, validating function signatures, and trapping type mismatches *before* code generation.
4. **Optimized Code Generator:** Generates valid `vasm6502_oldstyle` assembly. It avoids slow virtual-machine stack execution by mapping parameters and expression scratchpads directly onto a high-performance zero-page register design.

### Zero-Page Hardware-Register Layout

To maximize compilation density and execution speed, the code generator reserves and maps lower RAM (`$0000–$00FF`, **The Zero Page**) to form a virtual register file:

| Address Range | Identifier | Purpose |
| :--- | :--- | :--- |
| **`$00`** | `SP` | **Software Stack Pointer:** Tracks multi-byte local variable frames in main RAM. |
| **`$02` – `$1F`** | `r0` – `r14` | **Virtual Registers:** General 16-bit high-speed scratchpads for nested expression evaluation. |
| **`$20` – `$2F`** | `args0` – `args7` | **Function ABI Zone:** Rapid parameter passing into function bounds without stack overhead. |
| **`$30` – `$33`** | `src`, `dest` | **Blitting Pointers:** Dedicated 16-bit windows for hardware-accelerated memory block copies. |
| **`$34` – `$3F`** | `sys_flags` | **System Status Flags:** Global bitmasks for fast 65C02 bit-testing routines. |
| **`$40` – `$FF`** | `usr_space` | **User Space:** Globals and variable caching up to the programmers discretion. |

---

## Language Specifications

### Basic Types

- `u8` / `i8`: 8-bit integers (unsigned / signed)
- `u16` / `i16`: 16-bit integers (unsigned / signed)
- `void`: Function return types with no payload.
- Pointer Types: Explicit address variables (e.g., `u16 *addr`).

### Memory Mapped I/O (`reg`)

Hardware interface registers are pinned directly to absolute memory bounds. Interacting with them compiles straight to ultra-fast absolute addressing instructions (`STA`, `LDA`), bypassing memory allocation entirely.

```c
reg u8 PORTA @ 0x6001;
reg u8 PORTB @ 0x6000;
```

## Compilation Example

Given a valid .c02 source snippet:

```c
u16 monitor_addr = 0x8000;

fn read_byte(u16 addr) -> u8 {
  u8* ptr = (u8*)addr;
  return *ptr;
}

fn main() -> void {
  u8 x = 0;
  x = read_byte(monitor_addr);
}
```

The compiler evaluates scope boundaries, types, and emits optimized structural loops and operations targeting the vasm compiler output.

## Toolchain Usage

### Compiling the Compiler & Submodules

```shell
git submodule update --init --recursive
cd ext/vasm
make CPU=6502 SYNTAX=oldstyle
cd ../../

# Compile the C02 compiler binary
cargo build
```

### Memory Map Config File, `c02_config.ron`

The compiler will look for a `.ron` (Rusty Object Notation) file containing the memory map for the target in whatever base directory `cc02` gets invoked in. This file defines how the compiler will format the outputted assembly. This is a sample config that fits the specs of the Ben Eater Kit Computer:

```ron
Memory_Map(
  soft_stack_start: 0x0FFF, // can be any address that resolves to ram
  rom_start: 0x8000
)
```

### Running the Compilation Driver Script

The included cc02 wrapper script compiles your source code through the compiler pipeline, outputs an assembly file (.s), and automatically processes it via vasm into a bare-metal binary container (.bin).

```Bash
./cc02 test/valid.c02
```

If compilation, syntax checking, or type scoping fail at any point, detailed error locations are written back to stderr using editor-compatible tracking standards.

---

## Known Bugs

1. **Nested while + locals hang** — after an inner while exits, execution never reaches subsequent statements in the outer loop body. No-locals version unaffected. Root cause not yet isolated.
2. **`for` loops unimplemented** — tokenizer recognizes `Kw_for` but `parse_stmt` doesn't handle it. Will produce a parse error.
3. **Type casting unimplemented** — cast expressions (e.g. `(u8*)addr`) not yet handled in codegen.
4. **String literals unimplemented** — no parsing, or ROM placement for string data.
