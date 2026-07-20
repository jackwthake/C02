# Audit Findings — `c02-as` (C code generator)

**Status:** pinned to `main` @ `6d619f6` (2026-07-17), from a close read of
`c02-as/src/generator.c` + `ir.h`, with runtime verification in py65 via
`bin/c02c`. Format follows [`DEVIATIONS_hs_impl.md`](../DEVIATIONS_hs_impl.md);
companions: [frontend](FINDINGS_frontend.md), [linker](FINDINGS_linker.md),
[IR format](FINDINGS_ir_format.md).

Two frontend-owned miscompiles that *surface* here are documented in the
frontend doc and only cross-referenced below: `++`/`--` without store-back
([FE-19](FINDINGS_frontend.md#fe-19)) and nested field assignment into a temp
copy ([FE-20](FINDINGS_frontend.md#fe-20)) — in both, the generator faithfully
executes wrong IR.

`SPEC.md` §Scope excludes codegen, so most entries cite the spec only where a
*language-level* promise (a §Appendix B semantic, §4.2 interrupt behavior, an
accepted construct) is broken; the rest are
**none — undocumented architecture**.

**Verification level**: **Executed** = the exact program shown was compiled at
the pinned commit and run in py65 (expected vs. actual observed at
`PORTB`/$6000); **Source** = code-path analysis only.

Severity: **T** over-strict rejection · **P0** silent miscompile ·
**P1** loud failure/crash on a reasonable program · **P2** latent gap ·
**G** quirk.

## Quick Reference

| ID | Sev | Verified | Summary | Spec section |
|---|---|---|---|---|
| [CG-1](#cg-1) | P0 | Executed | Struct-by-value returns truncated to 1 byte | none — undocumented architecture |
| [CG-2](#cg-2) | P0 | Executed | Struct-by-value parameters copy 1 byte through a 2-byte ABI slot | none — undocumented architecture |
| [CG-3](#cg-3) | P0 | Executed | Pointer comparisons: 1 result byte written, 2 read back — control flow flips on ZP residue | [§6.3](../SPEC.md#63-binary-operators) |
| [CG-4](#cg-4) | P1 | Executed | `TAC_ADDR_OF` of a non-variable lvalue reads the operand union as a pointer → SIGSEGV | [§6.2](../SPEC.md#62-unary-prefix-operators) |
| [CG-5](#cg-5) | P0 | Executed | Zero-extension where sign-extension is needed: `ptr + (i8)-1`, i8 args into i16 params, i8 returns | [Appendix B](../SPEC.md#appendix-b-confirmed-runtime-semantics) |
| [CG-6](#cg-6) | P0 | Executed | Any `is_interrupt` function reached by `JSR` executes RTI against a call frame | [§4.2](../SPEC.md#42-interrupt-functions) |
| [CG-7](#cg-7) | P0 | Executed | String data emitted with `strlen`: embedded `\0` truncates; dedup coalesces by prefix | [§1.4](../SPEC.md#14-string-literals) |
| [CG-8](#cg-8) | P2 | Source | 32KB bounds check misses the footer: last 10 bytes ($FFF6–$FFFF) silently overwritten | none — undocumented architecture |
| [CG-9](#cg-9) | P2 | Source | `allocate_globals` never checks RAM_TOP — globals silently run past $3FFF | none — undocumented architecture |
| [CG-10](#cg-10) | P2 | Source | Interrupt handlers don't save the ABI zone, helper slots, or RET | none — undocumented architecture |
| [CG-11](#cg-11) | P2 | Source | Struct field offsets >255 wrap in `LDY #imm` on the pointer path | none — undocumented architecture |
| [CG-12](#cg-12) | P2 | Source | Every referenced global gets a dead ZP slot per function — wasted ZP, earlier exhaustion | none — undocumented architecture |
| [CG-13](#cg-13) | P2 | Source | Variable shift counts read only the low byte | none — undocumented architecture |

## Entries

### CG-1: Struct-by-value returns truncated to 1 byte

```c
struct Pair { u8 a; u8 b; }
fn make() -> Pair { return Pair{ .a = 7, .b = 42 }; }
fn main() -> void { Pair q = make(); PORTB = q.b; }
// PORTB == 0, expected 42 (Executed)
```

Two independent width errors, both from `codegen_type_size()` returning its
**default 1** for `TYPE_STRUCT` (it only knows scalars and pointers):

- `TAC_RETURN` (`generator.c:941`) copies
  `codegen_type_size(cfg->return_type)` = **1** byte into RET;
- the `TAC_CALL` return-copy (`:982`) copies the same 1 byte back out.

Fields past offset 0 of a returned struct are never transferred (`q.b` reads
whatever the destination temp held). Even with `full_type_size` substituted,
RET is a fixed 2-byte slot at $02/$03 — a ≥3-byte struct would overrun into
the ZP variable zone at $04 — so struct returns need a real ABI (RAM
scratch/hidden pointer), or the frontend/analyzer must reject them loudly.
Today they're accepted everywhere and silently wrong.

**Spec:** none — the ABI is undocumented architecture; the *language* accepts
struct return types (§3.1/§4.1 place no restriction).

**Verified:** Executed.

### CG-2: Struct-by-value parameters copy 1 byte through a 2-byte slot

```c
struct Pair { u8 a; u8 b; }
fn second(u8 dummy, Pair p) -> u8 { return p.b; }
fn main() -> void {
  Pair q = Pair{ .a = 7, .b = 42 };
  PORTB = second(0, q);          // PORTB == 7, expected 42 (Executed)
}
```

The call site copies `ABI_SLOT_SIZE` = 2 bytes of each arg into the fixed ABI
zone (`:972-978`) — already truncating any struct > 2 bytes — and the
prologue (`emit_function_prologue`, `:738`) then copies only
`codegen_type_size(param->type)` = **1** byte from the slot into the param's
ZP storage. `p.b` reads uninitialized ZP; the single-param variant of this
repro *passes by accident* because the callee's param slot happens to alias
the caller's argument ZP slot — one extra parameter breaks the coincidence,
which is exactly the kind of layout-dependent "works until it doesn't" P0.
Same fix space as CG-1: fixed 2-byte slots can never carry structs; pass big
values via RAM or reject struct params.

**Spec:** none — undocumented architecture (ABI).

**Verified:** Executed (both the failing layout and the accidental-pass one).

### CG-3: Pointer comparisons — one result byte written, two read back

```c
u8 pad = 1;   // moves g so &g's low byte is nonzero — this line flips the result
u8 g = 1;
fn check(u8 *p) -> u8 {
  if (p == null) { return 111; }
  return 222;
}
fn main() -> void { u8 *q = &g; PORTB = check(q); }
// PORTB == 111 (the p==null branch!) — remove `pad` and it's 222 (Executed)
```

Every comparison emitter ends `STA dst_zp` — exactly **one** boolean byte —
but the frontend types a comparison's destination temp at the *operand* type
when either operand is a pointer (`Lower.hs` widens/retypes only when both
operand depths are 0; §6.3 even blesses "result = wider operand type"). The
dst temp is therefore a 2-byte ZP slot whose high byte is never written, and
the very next consumer (`TAC_NOT` inside the `if`-lowering, or
`TAC_COND_JUMP`) sizes its read from that same type and `ORA`s the stale high
byte. The branch outcome depends on whatever previously occupied that ZP
address — above, the caller's `q` low byte. In py65's zeroed RAM many
programs pass; on real hardware (random ZP at power-on) this is
nondeterministic control flow on *any* pointer comparison (`p == null`,
`p != q`, `while (p < end)` …).

**Fix:** either the comparison emitters store `dst`'s full width (zero the
high byte), or the frontend types comparison results `(U8, 0)` uniformly —
the latter contradicts §6.3's "wider operand" wording, which was written for
scalars; the spec should say what a pointer comparison's type is.

**Spec:** §6.3 (result-type rule is ambiguous for pointers; the machine
behavior is undocumented architecture).

**Verified:** Executed — one unrelated global flips 222 → 111.

### CG-4: `TAC_ADDR_OF` of a non-variable lvalue — union misread → SIGSEGV

```c
struct Point { u8 x; u8 y; }
u8 g = 9;                       // any global makes lookup_global iterate
fn main() -> void {
  Point p = Point{ .x = 3, .y = 4 };
  u8 *q = &p.y;                 // c02-as: Segmentation fault (rc=139, Executed)
  PORTB = *q;
}
```

`TAC_ADDR_OF` (`:905`) does `lookup_global(e, instruction->src1.name)`
unconditionally. `&var` puts an `OPERAND_VAR` there and works; but `&p.y` and
`&*p` lower the operand as a value first ([the FE-19/FE-20 pattern]), so
`src1` is an **OPERAND_TEMP** — and `.name` aliases `.temp_id` in the operand
union. `strcmp` then dereferences a pointer forged from a small integer:
segfault whenever at least one global exists (`lookup_global` iterates), and
when no globals exist it "succeeds" by taking the **address of the temp's ZP
slot** — a pointer to a copy, silently wrong (P0-shaped fallback behind the
P1). Direct parallel to `cc02`'s
[P1-1](../DEVIATIONS_c_impl.md#p1-1-address-of-a-struct-field-segfaults-the-compiler),
reproduced in a new implementation. The analyzer accepts `&p.y` per §7.3
(`Field` is an lvalue), so this is reachable from ordinary source.

**Spec:** §6.2/§7.3 (language accepts it); crash behavior undocumented
architecture.

**Verified:** Executed — `c02-as` killed by SIGSEGV on the `.o`.

### CG-5: Zero-extension where sign-extension is required

```c
u8 a = 10;  u8 b = 20;
u8 *p = &b;
u8 *q = p + (i8)-1;   // q == p + 255, not p - 1 → *q reads 0, expected 10 (Executed)
```
```c
fn wide(i16 w) -> i16 { return w; }
i8 n = -5;
PORT16 = (u16)wide(n);  // 251 (0x00FB), expected 65531 (0xFFFB) (Executed)
```

`emit_load_byte` (and every `GLOBAL_AWARE_ALU_HELPER`) substitutes
`LDA #0` for any byte index past the operand's width — unconditional
**zero**-extension. That's correct for the unsigned widening of Appendix B,
and harmless for scalar binops because the frontend inserts explicit
`TAC_CAST`s (which *do* sign-extend, `:829-841`) before mixed-width scalar
ops. It is wrong on the three paths that skip the cast:

1. **Pointer arithmetic** — lowering deliberately doesn't widen the integer
   operand, so `TAC_ADD/TAC_SUB` on a pointer reads byte 1 of an `i8` offset
   as `$00`: a negative offset adds `+256+n` instead of `n`.
2. **Call arguments** — args are copied to ABI slots at 2 bytes with no cast;
   a negative `i8`/narrow arg into a wider signed param arrives
   zero-extended. (Admitted in the `generator.c:25` header comment — "a
   future IR pass should insert TAC_CAST nodes here" — but recorded in no
   deviations doc until now.)
3. **Return values** — `TAC_RETURN` loads `codegen_type_size(return_type)`
   bytes from the returned operand; `return n;` with `n : i8` from an
   `-> i16` function zero-extends the same way.

**Spec:** Appendix B pins unsigned widening as zero-extension; the *signed*
widening these paths perform is bit-pattern-wrong by §3.2 rule 8's model
(same-signedness widening must preserve value). Severity P0: analyzer accepts
all three shapes, output is silently wrong.

**Verified:** Executed (paths 1 and 2; path 3 Source, same mechanism).

### CG-6: Any `is_interrupt` function reached by `JSR` executes RTI against a call frame

```c
fn foo() interrupt -> void { PORTB = 42; }
fn main() -> void { foo(); PORTB = 7; }
// Runs foo's body, then never halts — PC ends at $0000; PORTB stays 42 (Executed)
```

`emit_function_from_cfg` gives every flagged CFG the PHA/PHX/PHY prologue and
PLY/PLX/PLA + **RTI** epilogue. RTI pops *status* then PC; a JSR pushed only
PC−1 — so the return address low byte is consumed as the status register and
execution resumes at a garbage address. Reachable two ways, both
analyzer-accepted: an invalid interrupt (wrong name — the frontend never
clears the flag, [FE-14](FINDINGS_frontend.md#fe-14), where §4.2 requires
warn-and-clear), and a *valid* `nmi`/`irq` handler called directly (§4.2 /
cc02 P2-3 says direct calls are currently accepted). The vector table itself
is handled correctly (`emit_vectors` only wires flagged `nmi`/`irq` — a
genuine improvement over cc02's name-only wiring); the crash is exclusively
the call path.

**Spec:** §4.2.

**Verified:** Executed — program never reaches the halt loop.

### CG-7: String data emitted with `strlen` — embedded NUL truncates

```c
u8 *s = "ab\0cd";
PORTB = *(s + 3);   // reads 67 ('C') — the "C02S" symbol-table magic that
                    // happens to follow the truncated string in ROM (Executed)
```

§1.4 defines the `\0` escape, and the wire format carries strings
length-prefixed with the NUL intact — but `emit_data_section` writes
`strlen(s)+1` bytes, so everything after the first embedded NUL is dropped,
and the ROM neighbor (here the symbol table) leaks into reads past it. The
same `strcmp`-based dedup coalesces any two literals equal up to the first
NUL ("ab\0cd" and "ab\0zz" share one address). The C-side loss of the length
is an IR-contract issue at root — see
[IR-2](FINDINGS_ir_format.md#ir-2) — but the observable miscompile is here.
(The `""` → NULL wire convention *is* handled — `u8 *s = ""; *s` yields 0,
Executed.)

**Spec:** §1.4.

**Verified:** Executed.

### CG-8: The 32KB bounds check misses the footer

`EMIT`/`PATCH_BYTE` guard every write against `ROM_SIZE` (that half of the
audit passes: all code, data, symbol-table, and fixup writes are covered —
fixup patch positions are only reachable un-overflowed because
`resolve_*_fixups` early-return on `e->overflow`, and the data/symtab phases
re-check between stages). What nothing guards is the **footer**: code+data may
legally grow through `$FFF6–$FFFF` (10 bytes: symtab pointer, boundary
pointer, NMI/Reset/IRQ vectors) without tripping `ROM_SIZE`, and
`emit_vectors` + the boundary-pointer store then overwrite whatever
instructions landed there — silently, since only the symbol table performs a
fit check against `SYMTABLE_START_PTR`. A program within ~10 bytes of exactly
32KB emits corrupted code with a success exit. The fix is one comparison:
treat `SYMTABLE_START_PTR` (or `0xFFFA−ROM_START` without symbols) as the
effective ceiling for `code_pos`/data.

**Spec:** none — undocumented architecture.

**Verified:** Source (requires a ≈32,758-byte image to demonstrate; the
arithmetic and write sites are all in `generate_rom`/`emit_vectors`).

### CG-9: No RAM-exhaustion check for globals

`allocate_globals` advances `e->ram_pos` from `$0200` with no comparison
against `RAM_TOP` (`$3FFF`): a large enough global set (or the compiler slots
appended by `emit_compiler_extern_inits`) walks into `$4000+` — unmapped/IO
space on the target board — and ultimately wraps `uint16_t`. `__heap_start`
is then also wrong (it's defined as first-free-RAM). All silent. ~15.5KB of
globals is unusual but reachable (a few large structs); the check is one line.

**Spec:** none — undocumented architecture (`docs/memmap.md` documents the
map but no stage enforces it).

**Verified:** Source.

### CG-10: Interrupt handlers preserve their own ZP map — and nothing else

`emit_zp_save` in a handler covers the handler's *own* var/temp slots, and
the prologue saves A/X/Y — correct as far as it goes. Not saved: the **ABI
zone** (`$EF–$FE`), the **arithmetic helper slots** (`$E0–$EC`), and **RET**
(`$02/$03`). An interrupt arriving between a caller's ABI-slot stores and its
`JSR` — or mid-`__mul8`/`__div16`, or between a callee's RTS and the caller's
RET read — has its state clobbered if the handler itself calls any function
with arguments, uses `*`//`/`/`%`, or calls anything non-void. That is most
non-trivial handlers. Runtime symptom: sporadic wrong argument/result values
in the interrupted code, unreproducible in step-through. Handlers must either
save `$E0–$FE` + `$02/$03` (≈33 extra pushes — stack budget!) or the docs
must ban calls/mul/div in handlers; today neither is done nor written down.

**Spec:** none — undocumented architecture (§4.2 covers only
declaration-site rules).

**Verified:** Source (py65 harness has no IRQ injection; the window is plain
from the emit order).

### CG-11: Field offsets past 255 wrap on the pointer path

`TAC_FIELD_LOAD/STORE` through a `Struct*` emit `LDY #(field->offset + b)`
with a `uint8_t` cast — a struct whose layout exceeds 256 bytes (nested
by-value structs make this easy: `struct Big { … }` at 130 bytes, containment
twice) silently wraps Y and reads/writes the wrong field. By-value bases are
safe in practice only because ZP slots cap locals well below 256, but a
*global* big struct accessed via pointer compiles and misbehaves. Cheap
detection at layout-read time (`total_size > 256` → error) would cover it.

**Spec:** none — undocumented architecture.

**Verified:** Source.

### CG-12: Globals get dead ZP slots in every function that touches them

`zp_map_build` registers **every** `OPERAND_VAR` in the instruction stream,
including names that `lookup_global` will resolve to RAM — those ZP slots are
then never used for the global's storage (loads/stores go absolute), but they
count against the `$04–$DF` budget, get saved/restored by every
callee's `emit_zp_save` (cycles + hardware-stack bytes per call), and hasten
the loud "ZP space exhausted" failure for functions that reference many
globals. (The slots *are* legitimately used as pointer scratch on the
`TAC_LOAD/STORE/FIELD_*` indirect paths — a fix must keep those.) Not wrong
output; a real capacity/perf trap.

**Spec:** none — undocumented architecture.

**Verified:** Source.

### CG-13: Variable shift counts read only the low byte

`TAC_SHL/TAC_SHR` with a non-constant count do `emit_load_byte(src2, 0)` into
X: a `u16` count of 256 shifts **zero** times (result = operand) instead of
producing 0. Constant counts unroll fully and are fine (a pathological
constant just overflows the ROM loudly). Narrow edge, silent when hit.

**Spec:** none — undocumented architecture (shift-by-≥-width semantics are
unspecified in `SPEC.md` generally).

**Verified:** Source.
