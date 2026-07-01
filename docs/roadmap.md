# C02 Roadmap & Design Notes

This document outlines the path from the current state of the compiler to
v1.0 and beyond. It covers feature milestones, the planned compilation
pipeline, and design sketches for major upcoming features.

## Compilation Pipeline

The full pipeline, once linking and optimization are in place:

```
                      ┌─────────────────────────────────────────┐
                      │          cc02 (single binary)           │
                      │                                         │
  foo.c02 ──► Tokenizer ──► Parser ──► Analyzer ──► IR Gen ──► foo.o
  bar.c02 ──► Tokenizer ──► Parser ──► Analyzer ──► IR Gen ──► bar.o
  baz.s   ──► Assembler ─────────────────────────────────────► baz.o
                      │                                         │
                      │  foo.o ─┐                               │
                      │  bar.o ─┼──► Linker ──► Optimizer ──► Codegen ──► out.bin
                      │  baz.o ─┘                               │
                      └─────────────────────────────────────────┘
```

Everything lives inside the `cc02` binary — there is no separate linker or
assembler executable. The driver detects input types and routes accordingly:

- `cc02 foo.c02 -o out.bin` — single-file: frontend → IR → codegen (today's path)
- `cc02 -c foo.c02 -o foo.o` — compile to IR object (today's `-c` flag)
- `cc02 foo.o bar.o -o out.bin` — link: deserialize, merge IR modules, optimize, codegen
- `cc02 foo.c02 bar.c02 -o out.bin` — compile + link in one invocation
- `cc02 -c baz.s -o baz.o` — assemble to object (future)

### Object file format

`.o` files currently serialize `ir_module_t` (IR-level objects). Linking
operates on IR, not machine code — the linker merges IR modules, then
hands the combined module to codegen. This is simpler than machine-code
linking (no relocation entries, no ELF-style sections) and means the
optimizer sees the whole program.

Native endianness, same-machine reuse only (documented in `ir-serial.c`).
A stable cross-platform format is not a priority until the IR stabilizes.

---

## v1.0 — Complete Single-File Language

The goal: someone can sit down and write a non-trivial 65C02 program
without hitting an "unimplemented" wall. All features the frontend accepts
should compile to correct code.

### v1.0 Feature Checklist

#### Must-have (language is broken without these)

- [x] **Function calls** — wire up the ABI zone (`$EF–$FF`), emit caller
      copies args into ABI slots before `JSR`, callee prologue copies from
      ABI into local ZP slots. Return values through `$02` (RET).
- [x] **Pointer store** (`*p = val`) — `TAC_STORE` with var/temp
      destination needs codegen via `STA ($nn),Y` indirect indexed.
      Currently silently no-ops (see BUG_REPORT.md BUG-2).
- [x] **Address-of** (`&x`) — `TAC_ADDR_OF` needs codegen. For ZP locals:
      load the ZP address as a constant. For globals: load the RAM address.
- [x] **Type casts / implicit widening** — `u8` → `u16` must zero-extend
      the high byte. Currently reads garbage.
- [x] Variables holding string have to be initialized as globals, this needs fixing
- [x] `*(ptr + i)` gives analyzer error, complaining that there is a type mismatch
      because ptr is a ptr and i is a normal integer value
- [x] Language currently does not support `break` or `continue` keywords

#### Should-have (language is painful without these)

- [x] **Struct field access codegen** — `TAC_FIELD_LOAD` / `TAC_FIELD_STORE`
      using the computed field offsets already in the IR. Base address + offset
      for by-value structs, indirect + offset for struct pointers.
- [x] **Multiply / divide / modulo** — no native 6502 instructions. Needs
      runtime helper routines (shift-and-add for multiply, repeated
      subtraction or restoring division for divide) emitted into ROM.
      8-bit first, 16-bit as a follow-up.
- [x] **Bitwise ops** (`&`, `|`, `^`, `~`) — straightforward: `AND`, `ORA`,
      `EOR` instructions, global-aware via the existing helper macro pattern.
- [x] **Shift ops** (`<<`, `>>`) — `ASL`/`LSR` for single-bit shifts,
      loop for multi-bit. Arithmetic right shift (`>>` on signed) needs
      sign-extension via `ROR` after checking the sign bit.

#### Nice-to-have (v1 can ship without)

- [ ] **Logical AND/OR short-circuit codegen** — the IR already lowers
      `&&`/`||` to labels and conditional jumps, but codegen needs to handle
      the resulting `TAC_AND`/`TAC_OR` if they appear in non-short-circuit
      contexts.
- [x] **Compound assignment codegen** (`+=`, `-=`, etc.) — these lower
      through the existing binary op + assignment IR path, so they may
      already work once the underlying ops are implemented. Needs testing.
      - [ ] `&=, |=, ^=, <<=, >>=` need implementing but arithmetic compound
            assignments work

### v1.0 Non-goals

- Multi-file linking (single-file programs only)
- Optimization passes
- Arrays / subscript syntax
- Full path-coverage return analysis

---

## v1.1 — Interrupt Handlers & Inline Assembly

These two features are tightly related: interrupt handlers need inline
assembly for the save/restore prologue (v1), and become fully automatic
once the codegen understands the `interrupt` calling convention.

### Interrupt handlers

Functions named `nmi` and `irq` are recognized by the codegen as interrupt
handlers:

```c
fn nmi() -> void interrupt {
  u8 status = IFR;   // read VIA to clear interrupt
}

fn irq() -> void interrupt {
  PORTB = 0xFF;
}
```

The `interrupt` keyword after the return type tells codegen:

1. **Emit `RTI` ($40) instead of `RTS` ($60)** — `RTI` restores the
   processor status register and PC from the stack, which `RTS` does not.
2. **Wire the vector table** — `emit_vectors` patches `$FFFA` (NMI) or
   `$FFFE` (IRQ) with the handler's ROM address instead of `$0000`.
3. **Auto-emit register save/restore** — the codegen wraps the function
   body with `PHA; TXA; PHA; TYA; PHA` (prologue) and
   `PLA; TAY; PLA; TAX; PLA` (epilogue) so the handler doesn't corrupt
   the interrupted code's A/X/Y registers.

The vector is matched by function name: `strcmp(cfg->name, "nmi")` and
`strcmp(cfg->name, "irq")`, the same way `main` is already identified for
`JSR main`. An `interrupt` function that isn't named `nmi` or `irq` still
gets `RTI` + register save/restore but isn't wired to a vector — useful
for BRK handlers or shared helper routines.

**ZP conflict note:** interrupt handlers and `main` currently share the ZP
register file (per-function maps both start at `$04`). The auto register
save/restore covers A/X/Y but not ZP locals. For v1.1 this is a documented
limitation — handlers should be short and avoid deep expression trees. A
future improvement could reserve a separate ZP region for interrupt
contexts or spill to RAM.

### Inline assembly

A minimal `asm {}` block that emits raw instructions at the current point
in the codegen output:

```c
fn wait() -> void {
  asm {
    SEI
    WAI
    CLI
  }
}
```

#### v1.1 scope (minimal)

- Instructions are opcode mnemonics with addressing modes, no operand
  constraints — the user writes literal addresses, not C02 variable names.
- The codegen parses each line, looks up the opcode/mode, and emits bytes
  through the existing `EMIT()` macro.
- No interaction with the ZP map or register allocator — the user is
  responsible for not stomping on compiler-managed ZP slots.
- Labels within `asm {}` blocks are local to the block.

```c
fn delay() -> void {
  asm {
    LDX #$FF
  loop:
    DEX
    BNE loop
  }
}
```

#### Future scope (post-v1.1)

- **Operand constraints** — let `asm` blocks reference C02 variables.
  The codegen resolves the variable's ZP/RAM address and substitutes it
  in. Syntax TBD but something simpler than gcc's constraint language:
  ```c
  asm { LDA {x}; STA $6000 }
  ```
- **Clobber declarations** — tell the compiler which registers the asm
  block modifies so it can save/restore around it.

---

## v1.2 — Multi-File Linking

Link multiple `.o` (serialized IR) files into a single ROM.

### Merge strategy

The linker stage deserializes each `.o` into an `ir_module_t` and merges
them into a single combined module:

1. **Concatenate** all `cfgs`, `globals`, `structs`, `regs` arrays.
2. **Resolve `decl` forward declarations** — match each `decl` against a
   concrete definition from another module. Error if unresolved or
   multiply defined.
3. **Deduplicate struct definitions** — same-named structs across modules
   must have identical layouts (field names, types, order). Mismatch is an
   error.
4. **Deduplicate register definitions** — same-named regs must have the
   same address and type.
5. **Hand the merged `ir_module_t` to `generate_rom`** — the existing
   codegen sees one big module, exactly like a single-file compile.

### Driver behavior

```bash
cc02 foo.o bar.o -o out.bin         # link objects
cc02 foo.c02 bar.c02 -o out.bin     # compile + link
cc02 foo.c02 bar.o -o out.bin       # mix of source and object inputs
```

The driver compiles any `.c02` inputs to IR (in memory, no temp file), then
merges all modules and runs codegen. This is the same model as `gcc foo.c
bar.c -o out` — the user doesn't need to know about the intermediate steps.

### Assembly object linking

Once the assembler exists (v1.1 inline asm or a standalone `.s` input),
`.s` files can be assembled to IR-level objects. An assembly function would
be represented as a `cfg_t` with a single basic block containing raw byte
data (a `TAC_RAW_BYTES` instruction or similar), so the linker and codegen
treat it like any other function — it has a name, it gets a ROM address,
JSR fixups resolve to it.

---

## v1.3+ — Optimization

Optimization runs on the merged IR, after linking and before codegen. A
single combined module means the optimizer sees the whole program — it can
inline across files, eliminate dead functions, and propagate constants
globally.

### Optimization pipeline

```
merged ir_module_t
  → constant folding        (replace 2+3 with 5)
  → dead code elimination   (remove unreachable blocks, unused globals)
  → function inlining       (small leaf functions)
  → copy propagation        (x = y; use x → use y, eliminate x)
  → ZP map compaction       (codegen: fewer slots, tighter addressing)
  → generate_rom()
```

Each pass transforms the IR in place and is independently toggleable
(`-O0` skips all, `-O1` runs the cheap ones, `-O2` runs everything).

### Why post-link, not per-file

Per-file optimization would mean optimizing each `.o` before merging, then
optimizing again after. This is how GCC/LLVM work (per-file opts + LTO),
but it only matters when compilation is slow enough that caching optimized
`.o` files saves time. For a 6502 target with small programs, the whole
compile-link-optimize-codegen pipeline runs in single-digit milliseconds.
Optimizing the combined IR once is simpler and strictly more powerful.

### Potential 6502-specific optimizations

- **ZP allocation coloring** — temporaries with non-overlapping lifetimes
  can share ZP slots, reducing ZP pressure.
- **Peephole patterns** — `LDA x; STA y; LDA y` → `LDA x; STA y` (the
  second LDA is redundant, A already holds the value).
- **Branch relaxation** — replace `JMP` (3 bytes) with `BRA` (2 bytes,
  65C02-only) when the target is within relative range.
- **Tail call optimization** — replace `JSR fn; RTS` with `JMP fn`.

---

## v2.0+ — Future Ideas

Not planned in detail, but worth noting as directions:

- **Arrays and subscript syntax** (`a[i]`) — needs a new type, bounds
  checking (optional), and indexed addressing mode codegen.
- **Standalone assembler input** (`cc02 -c foo.s`) — full `.s` file
  support, not just inline blocks. Reuses the inline asm parser.
- **Multiple ROM size targets** — not just 32K. Configurable via a
  linker script or command-line flag for different hardware setups.
- **Stable .o format** — versioned, endian-independent, so objects can
  be shared across machines.
- **Source-level debugging info** — emit address→line mappings so a
  debugger or emulator can show C02 source alongside execution.
