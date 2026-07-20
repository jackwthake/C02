# Audit Findings — `c02-frontend` (Haskell) vs. `SPEC.md`

**Status:** pinned to `main` @ `6d619f6` (2026-07-17), against `docs/SPEC.md` at
the same checkout. Companion to
[`DEVIATIONS_hs_impl.md`](../DEVIATIONS_hs_impl.md) (which already records
HS-1, untyped-literal signedness — not repeated here). Everything below is
**new**: found by auditing `C02.Parser.*`, `C02.Analyzer.*`, and
`C02.Lowering.*` against the spec, and verified as marked.

Where a finding concerns `include` resolution or the lowering contract, the
spec reference is **none — undocumented architecture**: `SPEC.md` defines
language semantics only; it contains no `include` construct and says nothing
about the IR emission pipeline.

**Verification level** per entry:
- **Executed** — reproduced with the exact snippet shown, compiled with
  `bin/c02-frontend` / run through `bin/c02c` + py65 at the pinned commit.
- **Source** — derived from reading the sources; re-verify before relying on
  it as an oracle.

Severity legend (same as `DEVIATIONS_hs_impl.md`):
- **T** — over-strictness: a program `SPEC.md` requires to be accepted is rejected.
- **P0** — silent miscompile: accepted by analysis, wrong machine code, no diagnostic.
- **P1** — loud failure: a stage crashes or fails unsafely on a reasonable program.
- **P2** — robustness: real gap, latent or needing an unusual program.
- **G** — grammar/lexer quirk.

## Quick Reference

| ID | Sev | Verified | Summary | Spec section |
|---|---|---|---|---|
| [FE-1](#fe-1) | T | Executed | Struct types defined in an included header are unusable in the includer (per-file prescan) | none — undocumented architecture |
| [FE-2](#fe-2) | P1 | Executed | Root file is never in the include visited-set: self-include / root↔header cycles duplicate the root's declarations | none — undocumented architecture |
| [FE-3](#fe-3) | P2 | Executed | No path canonicalization: the same header via two spellings is included twice | none — undocumented architecture |
| [FE-4](#fe-4) | G | Executed | include-not-found diagnostic loses the include site's position | none — undocumented architecture |
| [FE-5](#fe-5) | P2 | Source | Lazy `readFile` lets late I/O errors escape the resolver's `try` and crash the driver | none — undocumented architecture |
| [FE-6](#fe-6) | G | Source | Included declarations are hoisted above the includer's own, erasing include position | none — undocumented architecture |
| [FE-7](#fe-7) | T | Executed | No `asm { }` statement exists in the grammar | [§5.7](../SPEC.md#57-inline-assembly-asm) |
| [FE-8](#fe-8) | G | Executed | Char literals: an undocumented extension, not a lexeme (`'a' + 1` fails), Haskell escape rules | [§1.3](../SPEC.md#13-integer-literals) |
| [FE-9](#fe-9) | G | Executed | Adjacent operators fail to lex: `a*-b`, `a<-1`, `a&&&b` are parse errors | [§1.6](../SPEC.md#16-operators--punctuation) |
| [FE-10](#fe-10) | G | Executed | Integer literals overflowing the host word silently wrap (`2^64+1` lexes as `1`) | [§1.3](../SPEC.md#13-integer-literals) |
| [FE-11](#fe-11) | T | Executed | Compound assignment doesn't type-check as its desugaring: `p += 1` rejected on a pointer | [§5.3](../SPEC.md#53-assignment) |
| [FE-12](#fe-12) | P2 | Executed | S-1/S-17 laxity reproduced: any `(void*)`-shaped actual is compatible with *everything* | [§3.2](../SPEC.md#32-type-compatibility) |
| [FE-13](#fe-13) | P2 | Executed | `&&`/`||` operands are entirely unchecked (struct `&&` struct accepted) | [§6.3](../SPEC.md#63-binary-operators) |
| [FE-14](#fe-14) | P0 | Executed | `interrupt` validation missing: no warning, flag not cleared → calling the function crashes at runtime | [§4.2](../SPEC.md#42-interrupt-functions) |
| [FE-15](#fe-15) | G | Executed | `for`-increment clause accepts a variable declaration | [§5.5](../SPEC.md#55-for-loop-clauses) |
| [FE-16](#fe-16) | P2 | Source | Statement-position structs get no redeclaration check at all | [§7.2](../SPEC.md#72-scope-stack--shadowing) |
| [FE-17](#fe-17) | P1 | Executed | One name-keyed layout table across all scopes: a local `struct S` clobbers the top-level `S`'s layout for the whole module | none — undocumented architecture |
| [FE-18](#fe-18) | P0 | Executed | Non-literal global initializers silently lower to "uninitialized" | [§4.5](../SPEC.md#45-global-variables) |
| [FE-19](#fe-19) | P0 | Executed | `++`/`--` on a deref, field, or register mutates a loaded temp and never stores back | [§6.2](../SPEC.md#62-unary-prefix-operators) |
| [FE-20](#fe-20) | P0 | Executed | Nested field assignment (`a.b.c = v`) stores into a temporary copy of `a.b` | [§6.4](../SPEC.md#64-postfix--field-access) |
| [FE-21](#fe-21) | P2 | Source | The IR emission contract: what is promised to `c02-ld`/`c02-as` implicitly, with nothing checking it | none — undocumented architecture |
| [FE-22](#fe-22) | T | Executed | `__heap_start`/`__memory_top` are not injected — and `libc02`'s header declares the wrong type | [§4.6](../SPEC.md#46-forward-declarations-decl) |

## Entries

### FE-1: Struct types defined in an included header are unusable in the includer

```c
// inc/inner.h
struct Inner { u8 v; }

// main.c02
include "inc/inner.h";
fn take(Inner *p) -> void { }   // PARSE ERROR: "unexpected 'I', expecting ')'"
fn main() -> void {
  Inner x;                       // "`Inner` is not a struct type"
  struct O { Inner i; }          // parse error inside the field list
}
```

The struct-name prescan (`prescanStructNames`, SPEC §6.6) that drives every
type-vs-identifier disambiguation — declarations, casts, `for`-inits, params,
field types — runs **per file, before include resolution**. Include resolution
(`C02.Analyzer.Includes`) happens *after* the whole root file is parsed, so a
struct declared only in a header is not in the includer's prescan set and every
use of it as a type either hard-fails the parse (param/field position) or
misparses (`Inner x;` → not-a-struct; `Inner * p;` → multiplication → undeclared
identifiers).

Consequence: headers can share **functions, globals, and registers** (via
`decl` and definitions) but **not struct types**, which defeats half the point
of the header-file feature (added in #12). The only workaround is repeating the
`struct` definition in every file that names the type — which then trips the
linker's struct dedup (fine if layouts are identical, see
[LD-2](FINDINGS_linker.md#ld-2) for the local-struct hazard).

Fix direction: either prescan after splicing (resolve includes at the token or
text level before parse), or seed each included file's parse with the
accumulated struct-name set and re-parse the root with the full set.

**Spec:** none — `SPEC.md` has no `include` construct; §6.6 defines the prescan
as "whole-file", which this technically satisfies, but the multi-file design
intent is clearly violated.

**Verified:** Executed — both snippets above, exact diagnostics quoted.

### FE-2: Root file is never in the visited set — self-include and root↔header cycles duplicate the root

```c
// selfinc.c02
include "selfinc.c02";
u8 g = 1;
fn main() -> void { }
// → "redeclaration of 'g'", "redeclaration of 'main'"
```

`resolveIncludes` seeds the visited map empty; only files *read by the
resolver* are inserted. The root file's own path is never added, so any include
chain that reaches the root (directly, or via `a.c02 → b.h → a.c02`) re-reads
and re-splices the root's declarations. The cycle *does* terminate (the second
lap finds the intermediate paths visited), but the program then fails analysis
with spurious `redeclaration` errors for every top-level name in the root. The
mutual-cycle case (`cyc_a.c02 ↔ cyc_b.h`) reproduces identically.

**Spec:** none — undocumented architecture.

**Verified:** Executed — both the direct self-include and the two-file cycle.

### FE-3: No path canonicalization — one file, two spellings, two inclusions

```c
include "inc/decls.h";
include "./inc/decls.h";   // same file → "redeclaration of 'P' in this scope"
```

The visited-set key is the raw candidate path string (`dir </> name`), never
`canonicalizePath`/`normalise`d. `inc/decls.h` and `./inc/decls.h` (or the same
file reached via two different `-I` roots, `..` segments, or symlinks) are
distinct keys, so the pragma-once dedup fails and the declarations duplicate.
The same non-normalization also leaks into diagnostics (`././inc/decls.h:1:1`).

**Spec:** none — undocumented architecture.

**Verified:** Executed.

### FE-4: include-not-found diagnostic loses the include site

```
nope.h:1:1:
  |
1 | <empty line>
  | ^
```

`notFoundBundle` positions the error at offset 0 of a fabricated file named
after the *missing* header, with empty source text. The `Loc` of the `include`
statement is available in `resolveInclude` but is dropped before
`resolveCandidates`; the user never learns *which file, which line* did the
including — painful once headers include headers.

**Spec:** none — undocumented architecture.

**Verified:** Executed (rendering above; the search-path list does follow it).

### FE-5: Lazy `readFile` lets late I/O errors escape the resolver

`resolveCandidates` does `try (readFile path)`, but `readFile` is lazy: only
the *open* fails inside the `try`. A read/decode error surfacing while
`parseProgram` forces the string — the classic case being a byte sequence
invalid in the locale encoding (e.g. raw `0xFF` in a UTF-8 locale) — is thrown
outside the `ExceptT` channel and crashes `c02-frontend` with an uncaught
`IOException` instead of a diagnostic. A strict read (`readFile'` /
`Data.Text.IO`) or an encoding-fixed read closes both holes.

**Spec:** none — undocumented architecture.

**Verified:** Source — from the `try (readFile path)` shape in
`Includes.hs:100`; not executed.

### FE-6: Include position within the file is erased

`Program` separates the include list from the declaration list at parse time,
and `flatten` splices **all** included declarations ahead of **all** of the
includer's own — an `include` written mid-file or at the bottom behaves as if
written at the top. Observable consequences are limited today only because
FE-1 blocks the interesting case (the §4.4 by-value-containment ordering rule
is judged on the *post-splice* order, but a by-value field of a header struct
can't parse in the first place). If FE-1 is fixed, this silently changes
which §4.4 orderings are legal versus what the source text says.

**Spec:** none — undocumented architecture (§4.4's "earlier in the source
file" was written for a single file).

**Verified:** Source.

### FE-7: No `asm { }` statement

```c
fn main() -> void {
  asm { NOP }    // PARSE ERROR: "unexpected 'a', expecting '}'"
}
```

SPEC §5.7 defines the `asm` statement normatively (§2 grammar includes
`asm_stmt`; §1.1 reserves the keyword). This grammar has no production for it —
`asm` is reserved in the lexer (so it can't even be an identifier) but
`statementParser` has no branch. `c02-as` deliberately dropped its emitter
(noted in `generator.c`), so this looks like a chosen removal — but the spec
was never amended, `__enable_interrupts()` (§4.2, defined as `asm { CLI }`)
has no stated replacement, and there is no way to express SEI/CLI from source.
Until `SPEC.md` is amended this is a T deviation, and interrupt-driven
programs per §4.2 are not writable.

**Spec:** §5.7, §2, §4.2.

**Verified:** Executed.

### FE-8: Char literals — undocumented extension with two lexer quirks

SPEC §1.3 states flatly there are no character literals; this frontend added
them (`charLiteralParser`, commit `3484cad`). The extension itself needs a spec
amendment; on top of that:

1. **Not a lexeme.** `charLiteralParser` never consumes trailing whitespace, so
   any token after a char literal must be *adjacent*:

   ```c
   u8 c = 'a'+1;    // OK
   u8 c = 'a' + 1;  // PARSE ERROR: "unexpected space"
   ```

2. **Haskell escape semantics.** It delegates to megaparsec's `L.charLiteral`,
   which implements *Haskell* escapes — so `'\65'` (decimal), `'\x41'`, and
   `'\SOH'`-style names are accepted (`'\65'` compiles to 65, Executed), while
   `'\q'` is a parse error. String literals follow §1.4 (unknown escape drops
   the backslash: `"\q"` is `"q"`). The two literal forms disagree about what
   an escape is.

**Spec:** §1.3 (extension), §1.4 (escape inconsistency).

**Verified:** Executed — all four snippets.

### FE-9: Adjacent operators fail to lex

```c
i8 c = a*-b;     // PARSE ERROR at '*'
u8 d = a & ~b;   // OK — but a&~b is a PARSE ERROR
if (x<-1) { }    // PARSE ERROR
```

`operator` enforces maximal munch scannerlessly: after matching `*` it demands
the next char not be in the operator alphabet, so `*-`, `<-`, `&~`, `&&&`
all fail. A real tokenizer (§1.6, and `cc02`'s) lexes greedily per *token* —
`*-` is `*` then `-` since `*-` is not a token. Every binary-operator-followed-
by-unary-operator pairing without whitespace is a spurious parse error.
(`x=-1` is unaffected: assignment `=` goes through `symbol`, which has no
guard.)

**Spec:** §1.6 (token set; nothing there makes whitespace significant).

**Verified:** Executed (`a*-b`).

### FE-10: Host-word overflow in integer literals wraps silently

```c
u8 x = 18446744073709551617;   // ACCEPTED; x == 1 at runtime (2^64 + 1)
```

SPEC §1.3: a literal overflowing the host word is a **lexer error**. Here
`L.decimal` accumulates into `Int` and wraps modulo 2^64; the wrapped value
then passes the §3.4 range check. Confirmed end-to-end: the ROM writes 1.

**Spec:** §1.3.

**Verified:** Executed.

### FE-11: Compound assignment is not type-checked as its desugaring

```c
u8 x = 5;
u8 *p = &x;
p += 1;      // REJECT: "type mismatch in assignment: expected u8*, found u8"
p = p + 1;   // ACCEPT (§6.3 pointer arithmetic)
```

§5.3 says `lhs op= rhs` desugars to `lhs = lhs OP rhs`, whose RHS type is the
*binop result*. `analyzeStmt (Assign lhs _ rhs)` ignores the operator and
checks `rhs` directly against `lhs`'s type, so any compound assignment whose
bare RHS isn't already compatible with the LHS is rejected — pointer `+=`
integer being the everyday casualty. (The *lowering* desugars correctly; only
the analyzer's check is wrong, so this is pure over-strictness.)

**Spec:** §5.3 + §6.3.

**Verified:** Executed.

### FE-12: The S-1/S-17 laxity was reproduced deliberately — and is undocumented

```c
Point p = null;   // ACCEPTED (by-value struct from a "pointer")
u8 q = null;      // ACCEPTED
void *v = null;
u8 r = v;         // ACCEPTED (named void* value into a scalar)
```

`isTypeCompatible` rule 1 is implemented as "a `(Void, 1)` **actual** is
compatible with anything" — the code comment says this relaxation from §3.2's
pointer-only rule is deliberate, to match `cc02` (S-1/S-17). But `SPEC.md`
presents S-1/S-17 as *deviations to be fixed by this rewrite*, and
`DEVIATIONS_hs_impl.md` doesn't record that they were kept. Downstream,
`Point p = null;` lowers to a 2-byte copy of constant 0 into the struct
(partial zero-fill), and `u8 r = v;` truncates a pointer to one byte —
accepted silently.

Either amend §3.2 rule 1 to the relaxed reading, or restrict the rule to
pointer-typed `expected` (the type-level inability to distinguish the literal
`0` from a `void*` value is real, but the *destination* check costs nothing).

**Spec:** §3.2 rule 1 (and the S-1/S-17 write-ups it links).

**Verified:** Executed — all three accept.

### FE-13: `&&` / `||` skip operand checking entirely

```c
struct Point { u8 x; }
Point p;
u8 q = p && p;   // ACCEPTED
```

`checkBinary` returns `(U8, 0)` for `And`/`Or` before any compatibility check,
so struct, void-typed, and mixed-signedness operands all pass — every other
operator rejects them (§3.2's mixed-signedness rule; §6.3). Lowering then
emits truthiness tests over a struct operand (`TAC_NOT`/`TAC_COND_JUMP` on a
1-byte read of the struct), i.e. it "works" by testing the first field byte.
`cc02` runs `&&`/`||` operands through the same compatibility gate as other
binops; this is laxer than both spec and reference.

**Spec:** §3.2 (signedness note), §6.3.

**Verified:** Executed (accepts); lowering behavior Source.

### FE-14: `interrupt` validation is missing — and the consequence is a runtime crash, not a warning

```c
fn foo() interrupt -> void { PORTB = 42; }
fn main() -> void { foo(); PORTB = 7; }
// Compiles with NO output. At runtime: foo's body runs, then RTI against a
// JSR frame → PC ends at $0000; PORTB never becomes 7. (Executed in py65.)
```

§4.2: an `interrupt` function not named exactly `nmi`/`irq` (with `void`
return, zero params) must produce `WARN_INVALID_INTERRUPT` and compile as an
*ordinary* function with the flag cleared. This frontend performs **no
interrupt validation anywhere** (nothing in `Analyze.hs` touches
`isInterrupt`), and the flag rides through the IR to `c02-as`, which emits the
PHA/PHX/PHY prologue and PLY/PLX/PLA+RTI epilogue for any flagged function.
`c02-as` does correctly refuse to put a non-`nmi`/`irq` name in the vector
table — so the function is *only* reachable by a normal call, which the
analyzer permits (§4.2/P2-3), and every such call unbalances the hardware
stack and jumps to garbage. A one-word typo (`fn Nmi() interrupt`) is a silent
runtime crash instead of the spec's warning.

**Spec:** §4.2.

**Verified:** Executed — compile is silent, run never reaches the halt loop.

### FE-15: `for`-increment clause accepts a declaration

```c
for (u8 i = 0; i < 3; u8 j = 1) { }   // ACCEPTED; spec grammar: for_incr ::= expr (assign_op expr)?
```

`forClause` (shared by init and incr) includes `varDeclParser`; §5.5 permits a
declaration only in `for_init`. The declaration in incr position is then
re-declared-and-discarded once per iteration by the analyzer's
`analyzeStmt (LocVarDecl …)` no-op path and lowered as a per-iteration copy —
accepted, defined-ish behavior, but not the spec's language.

**Spec:** §5.5, §2 grammar.

**Verified:** Executed (accepts and compiles).

### FE-16: Statement-position structs get no redeclaration check

`declLocalStruct` does no same-scope duplicate check: `struct S { u8 a; }`
twice in one block silently takes the second definition. §7.2 puts structs in
the shared namespace (and even `cc02` catches same-scope struct redeclaration
— S-8 notes only the *shadowing* half is missing there). Combined with FE-17
this means the colliding definitions also fight over one layout slot.

**Spec:** §7.2.

**Verified:** Source.

### FE-17: One name-keyed layout table across all scopes

```c
struct S { u8 a; u8 b; }
fn main() -> void { S s; s.a = 1; s.b = 42; PORTB = s.b; }
fn other() -> void { struct S { u16 pad; u16 waste; } }
// → codegen: unknown field 'a' on struct 'S'   (exit 3; valid program)
```

`computeStructLayouts` folds **every** struct site — top-level and
function-local — into a single `Map String StructLayout` in source order, and
`lowerModule` serializes that map as *the* module struct table. Codegen
resolves every field access by name against that table. A local struct that
shares a top-level struct's name therefore replaces its layout for the entire
module: with different field names the program dies loudly at codegen (above);
with the *same* field names at different offsets it compiles silently against
the wrong layout (self-consistent within one module, but the table also leaks
into the linked object — see [LD-2](FINDINGS_linker.md#ld-2)). The analyzer,
meanwhile, resolves the same accesses scope-correctly, so the two stages
disagree about what `S` means.

**Spec:** none — undocumented architecture (the scoped-name → flat-table
lowering contract exists nowhere on paper).

**Verified:** Executed (the loud case).

### FE-18: Non-literal global initializers silently lower to "uninitialized"

```c
u8 g = 2 + 3;
fn main() -> void { PORTB = g; }   // PORTB == 0 at runtime, not 5 (Executed)
```

`toGlobal` captures only `IntLit`, `Unary Negate IntLit`, and `StrLit`; every
other accepted initializer shape — arithmetic, casts (`(u8)300`), `&global`,
struct initializers — becomes `IRInitNone` with **no diagnostic**. This is
exactly `cc02`'s P0-5, reproduced; the code comment calls it "a limitation,
not silently wrong data", but the analyzer accepted the program and the data
is silently wrong. Either reject non-literal global initializers loudly
(T-style strictness would at least be visible) or fold/emit them.

**Spec:** §4.5 (which documents P0-5 as a `cc02` deviation this rewrite was
meant to fix).

**Verified:** Executed.

### FE-19: `++`/`--` on a deref, field, or register never stores back

```c
u8 x = 41; u8 *p = &x;
++*p;            // x still 41 (Executed: PORTB reads 41)
++point.x;       // field still 10 (Executed)
++PORTB;         // register unchanged (Executed)
++x;             // plain local: works
```

`Lower.mutateUnary` lowers the operand as a *value* and emits
`TAC_INC/TAC_DEC` with that operand as `dst`. For a plain variable the operand
is the variable itself, so in-place mutation works. For `Deref`, `Field`, or a
register name, `lowerExpr` produces a **fresh temp holding the loaded value**
(`TAC_LOAD`/`TAC_FIELD_LOAD`), the increment hits the temp, and no store-back
is ever emitted. Three of the four lvalue shapes §6.2 requires `++`/`--` to
support are silent no-ops on memory.

**Spec:** §6.2/§7.3 (`++`/`--` on any lvalue).

**Verified:** Executed — all three broken shapes and the working one.

### FE-20: Nested field assignment stores into a temporary copy

```c
struct Inner { u8 v; }  struct Outer { Inner in; }
Outer o = Outer{ .in = Inner{ .v = 1 } };
o.in.v = 42;
PORTB = o.in.v;   // reads 1, not 42 (Executed)
```

`lowerStmt (Assign (Field base f) …)` lowers `base` as a *value*. When `base`
is itself a field access (`o.in`), that value is a `TAC_FIELD_LOAD` into a
temp — a byte-copy of the inner struct — and the `TAC_FIELD_STORE` then writes
into the copy. Any assignment whose target has ≥ 2 `.` levels (on by-value
structs) is silently lost. Pointer bases (`ptr.field = v` with one auto-deref
level) are unaffected since the "value" is the pointer.

**Spec:** §6.4 (chained field access), §5.3.

**Verified:** Executed.

### FE-21: The IR emission contract is implicit and unchecked

What `c02-frontend` promises the consumers (`c02-ld`, `c02-as`), none of it
written down or validated on either side — this is the inventory, all Source:

- **`nextTemp`/`nextLabel` are load-bearing.** `c02-as` allocates
  `next_label`-sized arrays and indexes them with instruction label ids
  unchecked; an id ≥ `next_label` is an out-of-bounds heap write inside the
  assembler (`Module.hs` even notes "codegen trusts them, so they must be
  right").
- **Exactly one block, id 0.** The wire format supports many blocks;
  the frontend always emits one and no consumer validates block structure.
- **`TAC_AND`/`TAC_OR`/`TAC_BREAK`/`TAC_CONTINUE` are never emitted** —
  `&&`/`||` become short-circuit control flow, break/continue become jumps
  (matching `bin/cc02`); `c02-as` has no cases for these four ops and fails
  loudly ("unhandled TAC op") if they ever appear.
- **Operand types are the sizing authority.** Codegen sizes every load/store
  from the serialized operand type; the frontend inserts explicit `TAC_CAST`s
  for binop widening but *not* for call arguments or return values (see
  [CG-5](FINDINGS_codegen.md#cg-5) for the runtime consequence).
- **Struct identity is by name only**, offsets pre-baked (see FE-17).
- **Only `__heap_start`/`__memory_top` may survive as extern variables** into
  `c02-as`; anything else is a codegen-time error, and nothing before codegen
  checks it (see [LD-7](FINDINGS_linker.md#ld-7)).

**Spec:** none — undocumented architecture.

**Verified:** Source (each point traced in `Lower.hs`/`Module.hs`/
`Serialize.hs` and the matching consumer code).

### FE-22: Compiler implicit globals are not injected — and the stdlib header's type disagrees with the spec

```c
fn main() -> void {
  PORT16 = __heap_start;   // "undeclared identifier '__heap_start'" (Executed)
}
```

§4.6 "Compiler Implicit Globals": `__heap_start` and `__memory_top` (both
`u16`) "are injected automatically — no `decl` needed, available in every
translation unit." This frontend injects nothing; the name resolves only if
the program writes the `decl` itself or picks it up from `libc02`'s
`stddef.c02h`. With an explicit `decl u16 __heap_start;` the whole pipeline
works (Executed — reads back the post-allocation RAM address via the
`c02-as` whitelist, [IR-7](FINDINGS_ir_format.md#ir-7)). Compounding it,
`bin/include/stddef.c02h` declares `decl u16 *__heap_start;` — a `u16*`,
where the spec says `u16` — so the *type* a stdlib-linked program sees also
deviates, and a `u16 h = __heap_start;` against the stdlib header is a type
mismatch the spec says should compile.

**Spec:** §4.6.

**Verified:** Executed (missing injection; the working `decl u16` control) /
Source (header type read from `libc02/include/stddef.c02h`).
