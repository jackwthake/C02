# C02 Roadmap & Design Notes

This document tracks what's next: upcoming feature milestones, the planned
compilation pipeline, and design sketches for unimplemented features.
Everything that's already shipped (including the full v1.0 feature
checklist) is recorded in `CHANGELOG.md` instead of here.


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

## Backburner

Nice-to-haves that were scoped for a milestone but didn't make the cut.
Nothing on the active roadmap depends on these — revisit opportunistically.

- [ ] **Logical AND/OR short-circuit codegen** *(deferred from v1.0)* — the
      IR already lowers `&&`/`||` to labels and conditional jumps, but
      codegen needs to handle the resulting `TAC_AND`/`TAC_OR` when they
      appear outside a bare condition context (e.g. `u8 x = a && b;` rather
      than `if (a && b)`).
- [ ] **`&=, |=, ^=, <<=, >>=` compound assignment** *(deferred from v1.0)*
      — arithmetic compound assignments (`+=`, `-=`, etc.) already work;
      the bitwise/shift forms still need implementing.

---

## v1.1 — Interrupt Handlers & Inline Assembly

Two independent features bundled into one milestone: hardware interrupt
support, and a minimal inline-assembly escape hatch. Turned out inline asm
wasn't a prerequisite for interrupts after all — register save/restore is
handled natively in codegen with real 65C02 push/pull instructions, no
`asm {}` block required. That feature is now independent, unstarted work.

### v1.1 Feature Checklist

#### Interrupt handlers *(complete)*

```c
reg u8 IFR @ 0x600D;   // VIA interrupt flag register — board-specific address,
                       // declared like any other peripheral register, not
                       // compiler-implicit (unlike __heap_start/__memory_top)

fn nmi() interrupt -> void {
  u8 status = IFR;   // read VIA to clear interrupt
}

fn irq() interrupt -> void {
  PORTB = 0xFF;
}
```

- [x] **`interrupt` qualifier** — parsed between the parameter list and
      `->`, on both definitions and forward declarations. `is_interrupt` is
      a real field threaded end-to-end (parser `node_t` → IR `cfg_t` →
      codegen) rather than codegen guessing from the function name.
      `--ast-dump` and `--ir-dump` both surface it
      (`interrupt fn nmi() -> void`).
- [x] **`RTI` ($40) instead of `RTS` ($60)** on every return path.
- [x] **Real register save/restore** — entry emits `PHA; PHX; PHY` before
      anything else (including the normal per-function ZP save, which
      itself clobbers `A`) touches a register; exit mirrors with
      `PLY; PLX; PLA` before `RTI`. `PHX`/`PHY`/`PLX`/`PLY` are genuine
      65C02 opcodes ($DA/$5A/$FA/$7A) — an earlier draft faked the pushes
      with `TXS`, which doesn't push anything, it just clobbers the
      hardware stack pointer and corrupts the return address the hardware
      auto-pushes on interrupt entry. Verified by an emulator test
      (`interrupt_registers_preserved` in `emu_test.py`) that fires a real
      NMI mid-loop via py65 and asserts SP/A/X/Y are bit-identical after
      the handler returns.
- [x] **Vector wiring gated on the qualifier, not just the name** —
      `emit_vectors` only patches `$FFFA` (NMI) / `$FFFE` (IRQ) with a
      function's address if it's *both* named `nmi`/`irq` *and* declared
      `interrupt`. A plain function that happens to be named `nmi` is left
      alone (vector stays `$0000`) instead of being wired to hardware as a
      function that ends in `RTS`.
- [x] **Bad signatures warn, not error, and get poisoned back to an
      ordinary function** — `pass1_register_globals` checks that an
      `interrupt`-qualified function is named `nmi`/`irq`, returns `void`
      (not `void*`), and takes zero parameters (the hardware never
      populates the ABI zone for an interrupt, so parameters would read
      garbage). A violation prints `WARN_INVALID_INTERRUPT` and resets
      `is_interrupt` to `0` on the AST node, matching the codebase's
      existing poisoning convention (see `TYPE_INVALID`) rather than
      cascading into a miscompile.
- [x] **Test coverage** — parser/IR golden dumps (`parser_interrupt.c02`,
      `ir_interrupt.c02`) plus five emulator tests covering register
      preservation, vector gating, and both invalid-signature warnings.

**ZP conflict note (resolved by the existing calling convention):** the
original concern here was that interrupt handlers and the code they
interrupt share the same per-function ZP region (maps both start at `$04`).
In practice this is already covered by the callee-saves ZP convention every
non-`main` function uses — an interrupt handler's own `emit_zp_save`/
`emit_zp_restore` preserves whatever was sitting in its claimed ZP slots
before the handler ran, the same protection a JSR'd function gives its
caller. The remaining real limitation is hardware-stack *depth*, not
correctness: every byte an interrupt handler saves (A/X/Y plus its ZP
slots) is pushed onto the same `$0100–$01FF` stack used for `JSR` return
addresses, so a deep call chain interrupted by a handler with many locals
could in principle exhaust the 256-byte stack. There's no overflow
detection for this yet.

#### Inline assembly *(not started)*

```c
fn wait() -> void {
  asm {
    SEI
    WAI
    CLI
  }
}

fn delay() -> void {
  asm {
    LDX #$FF
  loop:
    DEX
    BNE loop
  }
}
```

- [ ] Parse `asm { ... }` blocks: opcode mnemonics with addressing modes,
      no operand constraints — the user writes literal addresses, not C02
      variable names.
- [ ] Codegen parses each line, looks up the opcode/mode, and emits bytes
      through the existing `EMIT()` macro.
- [ ] No interaction with the ZP map or register allocator — the user is
      responsible for not stomping on compiler-managed ZP slots.
- [ ] Labels within `asm {}` blocks are local to the block.

#### Future scope (post-v1.1, not scoped yet)

- [ ] **Operand constraints** — let `asm` blocks reference C02 variables.
      The codegen resolves the variable's ZP/RAM address and substitutes
      it in. Syntax TBD but something simpler than gcc's constraint
      language: `asm { LDA {x}; STA $6000 }`
- [ ] **Clobber declarations** — tell the compiler which registers the asm
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
