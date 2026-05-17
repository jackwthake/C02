# C02 Compiler

A small compiler for the C02 language, implemented in Rust. It reads `.c02` source files, tokenizes them, parses the resulting token stream, and generates assembly for the *65C02 Microprocessor*.

## What it is

C02 is a simple, C-like language with basic types, functions, registers, and control flow. The compiler is designed as a minimal learning project rather than a full production tool.

## Features

- token-level source location tracking
- parser error reporting with file and line information
- support for functions, variable declarations, assignments, and control flow
- Memory Mapped I/O as a first class citizen

## Usage

Run the compiler with a `.c02` source file:

```bash
cargo run -- <path/to/file.c02>
```

If parsing fails, the compiler reports the error and the source location in a format compatible with terminal editors.

## Example

A basic C02 function looks like this:

```c
reg u8 PORTB @ 0x6000;

fn main() -> void {
  PORTB = 0x69; // set address 0x6000 to 0x69
  return;
}
```
