# C02 Compiler

[![CI](https://github.com/jackwthake/C02/actions/workflows/ci.yml/badge.svg)](https://github.com/jackwthake/C02/actions/workflows/ci.yml)

A systems language compiler for the C02 language, implemented in Rust. It reads `.c02` source files, tokenizes and parses them into an Abstract Syntax Tree (AST), runs a semantic analysis pass, and generates native machine code for the 65C02 microprocessor.

## What it is

C02 is a strongly typed, C-like systems programming language designed specifically for resource-constrained 8-bit microprocessors (6502 and 65C02). It eschews heavy runtimes or interpreted VMs, compiling directly down to tight bare-metal machine instructions. The compiler is portable across any 6502-based system—from hobbyist single-board computers to retro computing platforms—through configurable memory maps.

## Key Features & Architecture

The compiler is built as a complete multi-stage pipeline:

1. **Source Tracking Tokenizer:** Maps characters to discrete tokens while maintaining source locations (file, line, column) for robust compilation errors.
2. **Recursive Descent Parser:** Transforms the token stream into a structured AST, treating hardware registers and standard controls as first-class grammatical constructs.
3. **Lexically Scoped Semantic Analyzer:** Implements a type synthesizer and validation engine. It enforces a hierarchical symbol table structure to handle block scoping (`if/else`, `while`), tracking variable lifetimes, validating function signatures, and trapping type mismatches before code generation.
4. **Optimized Code Generator:** Generates valid 65C02 binaries. It avoids slow stack execution by mapping parameters and expression scratchpads directly onto a high-performance zero-page register design.
5. **Disassembler:** Decodes compiled `.bin` files back into annotated 65C02 assembly, resolving jump targets to named labels for readability. See [c02-objdump](c02-objdump/) for more information.

### Zero-Page Hardware-Register Layout

To maximize compilation density and execution speed, the code generator reserves and maps lower RAM (`$0000–$00FF`, **The Zero Page**) to form a virtual register file:

| Address Range | Identifier | Purpose |
| :--- | :--- | :--- |
| **`$00`** | `FP` | **Frame Pointer:** Tracks multi-byte local variable frames in main RAM. |
| **`$02`** | `RET` | **Return Register:** Where every function or conditional puts its return value. |
| **`$04` – `$1D`** | `r0` – `r13` | **Virtual Registers:** General 16-bit high-speed scratchpads for nested expression evaluation. |
| **`$1E` – `$2F`** | `args0` – `args9` | **Function ABI Zone:** Rapid parameter passing into function bounds without stack overhead. |
| **`$30` – `$FF`** | `usr_space` | **User Space:** Globals and variable caching at the programmer's discretion. |

---

## Language Specifications

### Basic Types

- `u8` / `i8`: 8-bit integers (unsigned / signed)
- `u16` / `i16`: 16-bit integers (unsigned / signed)
- `void`: Function return types with no payload.
- Pointer Types: Explicit address variables (e.g., `u16 *addr`).

### Memory Mapped I/O (`reg`)

Hardware interface registers are pinned directly to absolute memory addresses. Interacting with them compiles to absolute addressing instructions (`STA`, `LDA`), bypassing memory allocation entirely.

```c
reg u8 PORTA @ 0x6001;
reg u8 PORTB @ 0x6000;
```

## Compilation Example

Given a valid `.c02` source snippet:

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

The compiler evaluates scope boundaries and types, then emits optimized 65C02 machine code targeting the configured memory map.

## Toolchain Usage

### Compiling the Compiler

```shell
sudo apt install build-essential curl -y

# Official Rust install script
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

git clone https://github.com/jackwthake/C02.git
cd C02
make
```

### Memory Map Config File (`c02_config.ron`)

The compiler looks for a `.ron` (Rusty Object Notation) config file in whatever directory `C02` is invoked from. This defines the target memory map for your specific 6502 system. A sample config for the Ben Eater Kit Computer:

```ron
Memory_Map(
  rom_start: 0x8000
  rom_top: 0xFFFF
)
```

If no config file is found, the compiler falls back to a default memory map with a warning.

### Target System Configuration

The C02 compiler is designed to be portable across any 6502-based system. The memory map configuration allows you to define:

- ROM start and end addresses

For reference, see [memmap.md](memmap.md) for the Ben Eater 65C02 Kit Computer memory layout. You can create custom configs for other 6502 systems (Apple II, Commodore 64, custom boards, etc.) by adjusting the memory boundaries your `c02_config.ron`.
> The C02 compiler will automatically look for a `c02_config.ron` file in the directory its invoked in. To pass a config file in a different directory use the: `-c` or `--cfg` flag followed by the path.

### Running the Compiler

```bash
./C02 [OPTIONS] <FILE>
```

## Options

- `<FILE>`: The input source file (.c02) or binary file (.bin or .out when using -d).
- `-v`, `--verbose`: Print intermediate compiler stages (Tokens, AST, Symbol Table) to stdout.
- `-o`, `--output <PATH>`: Specify a custom path for the generated binary file (defaults to input file with a .bin extension).
- `-c`, `--cfg <CFG>`: Path to c02_config.ron file used for memory map definition, will look in cwd if not passed.
- `-h`, `--help`: Display the help message.
- `-V`, `--version`: Show the compiler version.

## Examples

**Compile a source file:**
`./C02 src/main.c02`

**Compile with a custom output name:**
`./C02 src/main.c02 -o build/firmware.bin`

**Disassemble a binary:**
`./C02 build/firmware.bin -d`

**Debug compiler stages:**
`./C02 src/main.c02 -v`

The same `c02_config.ron` is used in both compilation and disassembly to ensure addresses are consistent between compilation and disassembly.
