# Fix Priority — the P0/P1 subset of the toolchain audit

**Status:** pinned to `main` @ `6d619f6` (2026-07-17). Condensed from the four
full audit docs — [frontend](FINDINGS_frontend.md) /
[linker](FINDINGS_linker.md) / [codegen](FINDINGS_codegen.md) /
[IR format](FINDINGS_ir_format.md) — which together carry 50 entries across
every severity. This doc keeps only the **P0** (silent miscompile) and **P1**
(loud crash/failure on a reasonable program) entries: 11 P0 + 3 P1 = 14 source
findings, grouped into **9 root causes** below since several findings are the
same bug observed from two ends of the pipeline.

Nothing here is new analysis — every entry links back to its full write-up
(exact repro, expected vs. actual, code path). This doc only reorders and
groups for triage. Ordered by blast radius: how much ordinary code the bug
can hit, not how hard the fix is.

## 1. Struct-by-value has no real ABI

**[CG-1](FINDINGS_codegen.md#cg-1)**, **[CG-2](FINDINGS_codegen.md#cg-2)** — both P0, Executed.

`codegen_type_size()` returns a hardcoded **1** for any struct type — it only
knows scalars and pointers. Struct **returns** copy 1 byte into the 2-byte RET
slot; struct **parameters** copy 1 byte out of the 2-byte ABI slot. Any struct
with more than one meaningful byte, passed or returned by value, silently
loses everything past the first field. CG-2's single-param repro *passes by
accident* (ZP slot aliasing) — this can already be live in code that "works"
today for the wrong reason.

**Fix direction:** structs by value need real storage (RAM scratch or a
hidden out-pointer) — the 2-byte RET/ABI slots can never carry them. Until
that lands, the analyzer should reject struct-by-value params/returns loudly
rather than accept and truncate.

## 2. Zero-extension where sign-extension is required

**[CG-5](FINDINGS_codegen.md#cg-5)** — P0, Executed (2 of 3 paths) / Source (1 of 3).

`emit_load_byte` always zero-extends past an operand's width. Correct for
unsigned widening, and masked for ordinary scalar binops (the frontend inserts
sign-correct `TAC_CAST`s there) — but three paths skip the cast entirely:
pointer arithmetic with a signed offset, call arguments into a wider signed
parameter, and return values narrower than the declared return type. A
negative `i8` offset, argument, or return value is silently reinterpreted as
a large positive number. This is ordinary signed arithmetic across a function
boundary — not an edge case.

**Fix direction:** the header comment in `generator.c:25` already names the
fix — insert `TAC_CAST` at these three sites, matching what scalar binops
already get.

## 3. Pointer comparisons read a byte codegen never wrote

**[CG-3](FINDINGS_codegen.md#cg-3)** — P0, Executed.

Comparison emitters write one boolean byte (`STA dst_zp`); the frontend types
the destination as the *pointer's* width (2 bytes) whenever either operand is
a pointer. The next consumer (`TAC_NOT`, `TAC_COND_JUMP`) reads both bytes and
`ORA`s in whatever stale value occupied the never-written high byte. Branch
outcome for `p == null`, `p != q`, `while (p < end)` depends on ZP residue —
one unrelated global flipped a real test from 222 to 111. On real hardware
(random ZP at power-on) this is nondeterministic control flow on *any*
pointer comparison.

**Fix direction:** either comparison emitters zero the full destination width,
or the frontend types comparison results `(U8, 0)` uniformly regardless of
operand type.

## 4. Lvalues get loaded into a temp instead of stored through

**[FE-19](FINDINGS_frontend.md#fe-19)**, **[FE-20](FINDINGS_frontend.md#fe-20)** — both P0, Executed.
**[CG-4](FINDINGS_codegen.md#cg-4)** — P1, Executed — same root pattern, different consequence.

`Lower.hs` lowers a deref/field/register operand as a *value* before
mutating or storing into it. `++*p`, `++point.x`, `++PORTB` increment a
loaded temp and never write back (FE-19); `o.in.v = 42` on a chained
by-value field stores into a temp copy of `o.in` (FE-20) — both silent no-ops
on the actual memory. The same pattern reaches `c02-as`: `&p.y` lowers its
base as a temp, so `TAC_ADDR_OF`'s `src1` is an `OPERAND_TEMP` whose
`.name` field aliases `.temp_id` in the operand union — `strcmp` dereferences
a forged pointer and segfaults the compiler outright (CG-4) whenever a global
exists to trigger the lookup path; with no globals it "succeeds" by returning
the address of the temp, silently wrong instead of crashing.

**Fix direction:** all three are the same missing case in `Lower.hs` —
`Deref`/`Field`/register operands need to lower to an **address**, not a
loaded value, whenever they're a mutation target (`++`/`--`, assignment LHS,
or the operand of `&`).

## 5. `interrupt` is unimplemented at both ends

**[FE-14](FINDINGS_frontend.md#fe-14)** — P0, Executed. **[CG-6](FINDINGS_codegen.md#cg-6)** — P0, Executed.

§4.2 requires the analyzer to warn and clear the flag on any `interrupt`
function not named exactly `nmi`/`irq`; nothing in `Analyze.hs` touches
`isInterrupt` at all, so the flag rides through unconditionally. On the
codegen side, every flagged function gets an RTI epilogue — correct only for
real hardware interrupt entry. A normal `JSR` call to *any* interrupt-flagged
function (a typo'd name, or a valid `nmi`/`irq` handler called directly, both
analyzer-accepted) executes RTI against a call frame: it pops the JSR return
address as if it were a status register and jumps to a garbage PC. One typo
turns into a runtime crash with a silent, warning-free compile.

**Fix direction:** frontend needs the §4.2 validation (warn + clear the flag
off-vector); codegen's RTI epilogue is fine once nothing miscategorized can
reach it via `JSR`.

## 6. Non-literal global initializers vanish

**[FE-18](FINDINGS_frontend.md#fe-18)** — P0, Executed.

`toGlobal` only recognizes a bare int literal, a negated int literal, or a
string literal as a global initializer. Anything else the analyzer accepts —
`u8 g = 2 + 3;`, a cast, `&other_global`, a struct initializer — silently
becomes "uninitialized" with **no diagnostic**. This is `cc02`'s documented
P0-5, reproduced rather than fixed by the rewrite.

**Fix direction:** either constant-fold accepted initializer shapes at lower
time, or reject non-literal global initializers loudly (a T-severity
deviation is strictly better than a silent one).

## 7. String literals decay through `strlen`/`strcmp`

**[CG-7](FINDINGS_codegen.md#cg-7)** — P0, Executed. Root cause is
**[IR-2](FINDINGS_ir_format.md#ir-2)** (Source): the wire format carries
strings length-prefixed and binary-safe; `emit_data_section` throws the
length away and re-measures with `strlen`.

`"ab\0cd"` (a valid §1.4 literal) truncates in ROM at the embedded NUL;
reading past it returns whatever byte follows in the image (observed: the `C`
of the adjacent symbol-table magic). `strcmp`-based dedup also coalesces any
two literals that agree up to their first NUL. The `""` case is fine — the
one place a NULL/empty-length string is explicitly handled.

**Fix direction:** `emit_data_section` (and the dedup comparison) need to use
the length the wire format already carries instead of C-string primitives.

## 8. Include cycles duplicate the root file

**[FE-2](FINDINGS_frontend.md#fe-2)** — P1, Executed.

The include resolver's visited-set starts empty and only files *read by the
resolver* are ever inserted — the root file's own path is never added. Any
include chain that loops back to the root (direct self-include, or a
root↔header mutual cycle) re-splices the root's own declarations on top of
themselves, and the program fails with spurious `redeclaration` errors for
every top-level name. Termination is fine; the diagnostics are just wrong,
on a program whose only mistake was an include cycle.

**Fix direction:** seed the visited set with the root file's own path before
resolution starts.

## 9. One struct-layout table, keyed by bare name, across every scope

**[FE-17](FINDINGS_frontend.md#fe-17)** — P1, Executed.

`computeStructLayouts` folds every struct definition in a module — top-level
*and* function-local — into a single name-keyed map; the analyzer resolves
field accesses scope-correctly, but codegen's module struct table only has
room for one `S`. A function-local `struct S` with different fields than the
top-level `S` clobbers the layout for the whole module: with different field
names this is a loud, confusing codegen-stage error ("unknown field 'a' on
struct 'S'") on an otherwise-valid program; with the same field names at
different offsets it's silently wrong instead. (The same flat table also
leaks into the linker — [LD-2](FINDINGS_linker.md#ld-2).)

**Fix direction:** struct identity needs to be scope-qualified (mangled name
or per-function table), not a bare source name, before it's serialized.

---

## What's deliberately not here

The other 36 entries (6 T, 7 G, 21 P2, 2 architecture-note) are real and worth
reading in the full docs, but none of them produce wrong output on ordinary
code without an unusual trigger (a ~32KB-exact ROM, a >255-byte struct behind
a pointer, an interrupt-driven concurrent access, a non-Latin-1 string
literal, etc.) — that's what makes them P2 rather than P0. **[LD-8](FINDINGS_linker.md#ld-8)**
is worth a second look separately: it's a prioritized list of test-suite gaps,
not a bug, and the top item (diamond-include topology) would have caught
LD-1 automatically.
